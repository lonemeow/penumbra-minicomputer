/*
 * boot_rom.c — Penumbra boot ROM
 *
 * Boot sequence: traps → RAM detect → inject UART device → autoconfig
 * → SD card probe → monitor loop. All discovered state lives in the
 * boot data tagged list at BOOTDATA_BASE (page 0, after vectors).
 * No globals — ROM has no writable data section.
 */

#include "console.h"
#include "sdcard.h"
#include "fat32.h"
#include "elf.h"
#include "util.h"
#include "bootdata.h"
#include "libc.h"
#include "penumbra.h"

typedef void (*trap_handler)(void);

#define TRAP_VECTORS ((volatile trap_handler *)0x00000000)

#define TRAP_BUS_FAULT 0
#define TRAP_EXT_IRQ   1
#define TRAP_TLB_MISS  2
#define TRAP_TLB_PROT  3
#define TRAP_PRIV_INST 4
#define TRAP_SYSCALL   5
#define TRAP_BREAK     6
#define TRAP_ILL_INST  7
#define TRAP_ALIGN_FLT 8

#define NUM_TRAPS      16

static const char *trap_name(int trapno) {
    switch (trapno) {
    case 0: return "BUS FAULT";
    case 1: return "IRQ";
    case 2: return "TLB MISS";
    case 3: return "TLB PROT";
    case 4: return "PRIV";
    case 5: return "SYSCALL";
    case 6: return "BREAK";
    case 7: return "ILLEGAL";
    case 8: return "ALIGN";
    default: return "UNKNOWN";
    }
}

/* Called from assembly trampolines in trap_entry.s.
 * SPR/sysreg reads done here via inline asm (penumbra.h) so we can
 * easily add diagnostics without touching assembly. */
void unhandled_trap(int trapno) {
    uint32_t epc        = penumbra_read_spr(SPR_EPC);
    uint32_t esr        = penumbra_read_spr(SPR_ESR);
    uint32_t fault_addr = penumbra_read_sysreg(SYSDEV_MMU, MMU_FAULT_ADDR);
    uint32_t fault_stat = penumbra_read_sysreg(SYSDEV_MMU, MMU_FAULT_STATUS);

    console_printf("\r\nTRAP %d (%s) pc=0x%x", trapno, trap_name(trapno), epc);
    console_printf(" sr=0x%x fa=0x%x fs=0x%x\r\n", esr, fault_addr, fault_stat);

    /* Trampoline executes BREAK after we return, halting the simulator. */
}

/* Assembly trampolines — defined in trap_entry.s */
extern void _trap_bus_fault(void);
extern void _trap_irq(void);
extern void _trap_tlb_miss(void);
extern void _trap_tlb_prot(void);
extern void _trap_priv(void);
extern void _trap_syscall(void);
extern void _trap_break(void);
extern void _trap_illegal(void);
extern void _trap_align(void);

static void setup_traps(void) {
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_fault;
    TRAP_VECTORS[TRAP_EXT_IRQ]   = _trap_irq;
    TRAP_VECTORS[TRAP_TLB_MISS]  = _trap_tlb_miss;
    TRAP_VECTORS[TRAP_TLB_PROT]  = _trap_tlb_prot;
    TRAP_VECTORS[TRAP_PRIV_INST] = _trap_priv;
    TRAP_VECTORS[TRAP_SYSCALL]   = _trap_syscall;
    TRAP_VECTORS[TRAP_BREAK]     = _trap_break;
    TRAP_VECTORS[TRAP_ILL_INST]  = _trap_illegal;
    TRAP_VECTORS[TRAP_ALIGN_FLT] = _trap_align;
}

extern void _trap_bus_ignore(void);

/*
 * Probe the last word of a page to test if it is backed by RAM.
 * Write a pattern, read it back — if it matches, the page exists.
 * If not (bus fault), _trap_bus_ignore advances EPC and we return 0.
 */
static int detect_page(long pagenum) {
    volatile unsigned int *addr =
        (volatile unsigned int *)(((pagenum + 1) << 12) - 4);
    *addr = 0x12345678;
    return *addr == 0x12345678;
}

static long detect_ram(void) {
    void (*prev_vector)(void) = TRAP_VECTORS[TRAP_BUS_FAULT];
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;

    /* Pages 0-1 are reserved (vector table + boot data, stack) */
    long npages = 2;
    while (detect_page(npages))
        npages++;

    TRAP_VECTORS[TRAP_BUS_FAULT] = prev_vector;
    return npages;
}

/*
 * Autoconfig — enumerate devices on the bus via config chain.
 *
 * Appends BTAG_DEVICE entries to the boot data list for each
 * discovered device. Returns the number of devices found.
 */
static const char *class_name(uint32_t cls) {
    switch (cls) {
    case ACFG_CLASS_MEMORY: return "Memory";
    case ACFG_CLASS_UART:   return "UART";
    case ACFG_CLASS_SPI:    return "SPI";
    case ACFG_CLASS_SD:     return "SD";
    default:                return "Unknown";
    }
}

/*
 * Try to read a word from addr. If the read bus-faults (no device
 * responds), _trap_bus_ignore skips the LDW and the register keeps
 * the sentinel value 0xFFFFFFFF.
 */
static uint32_t bus_probe_read(uint32_t addr) {
    uint32_t val;
    asm volatile(
        "lli %0, #0xFFFF\n\t"
        "lui %0, #0xFFFF\n\t"
        "ldw %0, [%1]"
        : "=&r"(val) : "r"(addr) : "memory"
    );
    return val;
}

static int autoconfig(uint32_t *cursor) {
    int ndevs = 0;
    uint32_t next_io_addr = 0xFF001000;

    /* Assert bus reset to clear any stale device configs */
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_RST);

    /* Hold reset pulse >= 100 µs for external async bus devices */
    for (volatile int i = 0; i < BUS_RESET_DELAY_ITERS; i++) {}

    /* Deassert reset, enable config mode */
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_CFG_EN);

    /* Install bus-fault-ignore handler for probing */
    void (*prev_vector)(void) = TRAP_VECTORS[TRAP_BUS_FAULT];
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;

    for (;;) {
        /* Probe: read CFG_CLASS, sentinel 0xFFFFFFFF means bus fault */
        uint32_t cls = bus_probe_read(AUTOCONFIG_BASE + 0x00);
        if (cls == 0xFFFFFFFF)
            break;

        uint32_t size = bus_probe_read(AUTOCONFIG_BASE + 0x04);
        uint32_t id   = bus_probe_read(AUTOCONFIG_BASE + 0x08);

        /* Read device name (4 words → 16 bytes, word-aligned) */
        uint32_t name_words[5];
        name_words[0] = bus_probe_read(AUTOCONFIG_BASE + 0x0C);
        name_words[1] = bus_probe_read(AUTOCONFIG_BASE + 0x10);
        name_words[2] = bus_probe_read(AUTOCONFIG_BASE + 0x14);
        name_words[3] = bus_probe_read(AUTOCONFIG_BASE + 0x18);
        name_words[4] = 0;
        char *name = (char *)name_words;

        /* Allocate base address and configure the device */
        uint32_t base;
        if (cls == ACFG_CLASS_MEMORY) {
            base = 0x01000000;  /* TODO: track actual system RAM end */
        } else {
            uint32_t mask = size - 1;
            base = (next_io_addr + mask) & ~mask;
            next_io_addr = base + size;
        }
        *(volatile uint32_t *)(AUTOCONFIG_BASE + 0x1C) = base;

        /* Toggle CFG_EN so the chain settles before probing the
         * next device. Without this, the newly-configured device's
         * cfg passthrough could race with the write. */
        penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, 0);
        penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_CFG_EN);

        bd_add_device(cursor, cls, base, size, id, name);

        console_printf("  %s %s @ 0x%x (%d bytes)\r\n",
                        name, class_name(cls), base, (int)size);

        ndevs++;
    }

    /* Restore and disable config mode */
    TRAP_VECTORS[TRAP_BUS_FAULT] = prev_vector;
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, 0);

    return ndevs;
}

/* ── Monitor commands ─────────────────────────────────────────────── */

/*
 * cmd_examine — hex dump memory at a given address.
 *
 * Usage: x <addr> [<length>]
 * Displays 16 bytes per line: hex on the left, ASCII on the right.
 * Reads byte-at-a-time (LDB) so it works on any alignment/MMIO.
 */
static void cmd_examine(const char *args) {
    const char *p = args;
    unsigned long addr = strtoul(p, &p, 16);
    unsigned long len  = strtoul(p, &p, 10);
    if (len == 0) {
        len = 16;
    }
    int roundup_len = len + (16 - (len % 16)) % 16;
    volatile unsigned char *mem_ptr = (unsigned char *)addr;

    char hexbuffer[17];

    for (int i=0; i<roundup_len; i++) {
        if ((i & 0xF) == 0) {
            console_printf("%08x   ", (unsigned long)mem_ptr);
        }
        if (i < len) {
            unsigned char val = *mem_ptr++;

            console_printf(" %02x", val);

            if (isprint(val)) {
                hexbuffer[i & 0xF] = val;
            } else {
                hexbuffer[i & 0xF] = '.';
            }
        } else {
            console_puts("   ");
                hexbuffer[i & 0xF] = ' ';
        }
        if ((i & 0xF) == 0xF) {
            hexbuffer[16] = '\0';
            console_printf(" | %s\r\n", hexbuffer);
        }
    }
}

/*
 * cmd_load — read sectors from SD card into memory.
 *
 * Usage: load sd:<dev>,<cs>[:<part>] <addr> <lba> <count>
 *
 * Without :<part>, LBA is absolute (raw card access).
 * With :<part> (1-based), LBA is relative to the partition start.
 *
 * Full lifecycle per command: init → read sectors → deinit.
 * Card can be swapped between load commands.
 */
static void cmd_load(const char *args) {
    const char *p = args;

    /* Parse sd:<dev>,<cs>[:<part>] */
    if (!(p[0] == 's' && p[1] == 'd' && p[2] == ':')) {
        console_puts("usage: load sd:<dev>,<cs>[:<part>] <addr>"
                      " <lba> <count>\r\n");
        return;
    }
    p += 3;
    unsigned long dev_nth = strtoul(p, (char **)&p, 10);
    if (*p == ',') p++;
    unsigned long cs = strtoul(p, (char **)&p, 10);

    int partition = -1;   /* -1 = raw/whole device */
    if (*p == ':') {
        p++;
        partition = (int)strtoul(p, (char **)&p, 10);
    }
    while (*p == ' ') p++;

    unsigned long addr  = strtoul(p, (char **)&p, 16);
    unsigned long lba   = strtoul(p, (char **)&p, 10);
    unsigned long count = strtoul(p, (char **)&p, 10);

    if (count == 0) {
        console_puts("usage: load sd:<dev>,<cs>[:<part>] <addr>"
                      " <lba> <count>\r\n");
        return;
    }

    struct btag_device *dev =
        bd_find_device_by_class(ACFG_CLASS_SD, (int)dev_nth);
    if (!dev) {
        console_printf("sd:%d — no such SD controller\r\n", (int)dev_nth);
        return;
    }

    (void)cs;  /* TODO: support CS1 */

    /* If a partition was specified, read MBR for the LBA offset */
    uint32_t lba_offset = 0;
    if (partition >= 0) {
        uint32_t part_start, part_size;
        int prc = mbr_get_partition(dev->base, partition,
                                    &part_start, &part_size);
        if (prc == -1) {
            console_puts("SD init failed (MBR read)\r\n");
            return;
        } else if (prc == -2) {
            console_puts("failed to read sector 0\r\n");
            return;
        } else if (prc == -3) {
            console_puts("no valid MBR signature\r\n");
            return;
        } else if (prc == -4) {
            console_printf("partition %d: empty or invalid\r\n", partition);
            return;
        }

        if (lba + count > part_size)
            console_printf("warning: read extends past partition end"
                           " (%d sectors)\r\n", (int)part_size);

        lba_offset = part_start;
    }

    /* Init card fresh each time (supports hot-swap) */
    int rc = sd_init(dev->base);
    if (rc != 0) {
        console_printf("SD init failed (err=%d)\r\n", rc);
        return;
    }

    unsigned char *dst = (unsigned char *)addr;
    int err = 0;
    for (unsigned long i = 0; i < count; i++) {
        if (sd_read_sector(dev->base, lba_offset + lba + i, dst) != 0) {
            console_printf("read error at LBA %d\r\n",
                            (int)(lba_offset + lba + i));
            err = 1;
            break;
        }
        dst += 512;
    }

    sd_deinit(dev->base);

    if (!err)
        console_printf("loaded %d sectors (%d bytes) to 0x%x\r\n",
                        (int)count, (int)(count * 512), (unsigned int)addr);
}

/*
 * cmd_part — display MBR partition table from an SD card.
 *
 * Usage: part sd:<dev>,<cs>
 */
static void cmd_part(const char *args) {
        if (strncmp(args, "sd:", 3)) {
        console_puts("usage: part sd:<dev>,<cs>\r\n");
        return;
    }

    const char *p = args + 3;
    unsigned long dev_nth = strtoul(p, (char **)&p, 10);
    if (*p == ',') p++;
    unsigned long cs = strtoul(p, (char **)&p, 10);

    struct btag_device *dev =
        bd_find_device_by_class(ACFG_CLASS_SD, (int)dev_nth);
    if (!dev) {
        console_printf("sd:%d — no such SD controller\r\n", (int)dev_nth);
        return;
    }

    int rc = sd_init(dev->base);
    if (rc != 0) {
        console_printf("SD init failed (err=%d)\r\n", rc);
        return;
    }

    (void)cs;  /* TODO: support CS1 */

    unsigned char mbr[512];
    if (sd_read_sector(dev->base, 0, mbr) != 0) {
        console_puts("Error reading SD card\r\n");
        goto out;
    }

    unsigned short mbr_sig = read_le16(mbr + MBR_SIG_OFFSET);
    if (mbr_sig != MBR_SIGNATURE) {
        console_puts("No MBR signature found\r\n");
        goto out;
    }

    console_printf("MBR partition table on device %s card %d:\r\n\r\n", dev->name, cs);

    for (int part_idx=0; part_idx<4; part_idx++) {
        unsigned char *part_data = mbr + MBR_PART_OFFSET + part_idx * MBR_PART_ENTRY_SIZE;

        unsigned char part_status = part_data[0];
        unsigned char part_type = part_data[4];
        unsigned long part_start_lba = read_le32(part_data + 8);
        unsigned long part_sector_count = read_le32(part_data + 12);

        char humanized_size[32];
        humanize_size(part_sector_count * 512, humanized_size, sizeof(humanized_size));

        console_printf("  %d %10s %d %s %s\r\n", part_idx + 1, part_type_name(part_type),
            part_start_lba, humanized_size, part_status & 0x80 ? "(bootable)" : "");
    }

out:
    sd_deinit(dev->base);
}

/* ── FAT32 block-read adapter for SD ─────────────────────────────── */

static int sd_blk_read(uint32_t lba, unsigned char *dst, void *ctx)
{
    uint32_t base = (uint32_t)(unsigned long)ctx;
    return sd_read_sector(base, lba, dst);
}

/*
 * cmd_boot — load and execute the boot loader from a FAT32 partition.
 *
 * Usage: boot sd:<dev>,<cs>[/file]
 *
 * Finds the first FAT32 partition, mounts it, loads an ELF PIE
 * from the root directory, copies PT_LOAD segments to a chosen RAM
 * address, and jumps to the entry point with R1 = boot data.
 * Optional /filename overrides the default (PENBOOT.ELF).
 *
 * The LOADER is a PIE ELF that self-relocates at startup using a
 * small CRT stub.  The ROM just needs to load segments and jump.
 */
#define BOOT_FILENAME "PENBOOT.ELF"

/*
 * Validate an ELF32 header for Penumbra PIE.
 * Returns 0 on success, prints an error and returns -1 on failure.
 */
static int elf_validate(const struct elf32_ehdr *ehdr) {
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        console_puts("Not an ELF file\r\n");
        return -1;
    }
    if (ehdr->e_ident[4] != ELFCLASS32 || ehdr->e_ident[5] != ELFDATA2LSB) {
        console_puts("Not ELF32 little-endian\r\n");
        return -1;
    }
    if (ehdr->e_machine != EM_PENUMBRA) {
        console_printf("Wrong ELF machine: 0x%x\r\n",
                        (unsigned int)ehdr->e_machine);
        return -1;
    }
    if (ehdr->e_type != ET_DYN) {
        console_puts("Not a PIE (ET_DYN) ELF\r\n");
        return -1;
    }
    if (ehdr->e_phnum == 0) {
        console_puts("No program headers\r\n");
        return -1;
    }
    return 0;
}

/*
 * Compute total memory footprint of PT_LOAD segments.
 * Returns the span from lowest vaddr to highest vaddr+memsz.
 * Also stores the lowest vaddr in *base_vaddr.
 */
static uint32_t elf_memsz(const unsigned char *file,
                          const struct elf32_ehdr *ehdr,
                          uint32_t *base_vaddr) {
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    int i;
    for (i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph =
            (const struct elf32_phdr *)(file + ehdr->e_phoff +
                                        i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_vaddr < lo)
            lo = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > hi)
            hi = ph->p_vaddr + ph->p_memsz;
    }
    *base_vaddr = lo;
    return hi - lo;
}

/*
 * Find a contiguous RAM region of at least `size` bytes that does not
 * overlap any reserved region.  Walks ACFG_CLASS_MEMORY devices in
 * the boot data.  Returns a page-aligned (4096) address, or 0 on
 * failure.
 *
 * `reserved` is a NULL-terminated array of pointers to reserved
 * regions (vectors/stack, scratch buffer, etc.).
 */
struct reserved_region {
    uint32_t base;
    uint32_t size;
};

static uint32_t find_memory_region(uint32_t size,
                                   struct reserved_region **reserved) {
    int i = 0;
    while (1) {
        struct btag_device *ram_dev =
            bd_find_device_by_class(ACFG_CLASS_MEMORY, i);
        if (!ram_dev)
            return 0;

        uint32_t cand_start = ram_dev->base;
        uint32_t cand_end = cand_start + ram_dev->dev_size;

        int j;
        for (j = 0; reserved[j]; j++) {
            uint32_t res_start = reserved[j]->base;
            uint32_t res_end = res_start + reserved[j]->size;

            if (res_start <= cand_start && res_end >= cand_end) {
                /* Completely covered */
                cand_start = cand_end;
            } else if (res_start <= cand_start && res_end > cand_start) {
                /* Overlaps at start */
                cand_start = res_end;
            } else if (res_start < cand_end && res_end >= cand_end) {
                /* Overlaps at end */
                cand_end = res_start;
            } else if (res_start > cand_start && res_end < cand_end) {
                /* Splits the block; take the larger side */
                if ((res_start - cand_start) >= (cand_end - res_end))
                    cand_end = res_start;
                else
                    cand_start = res_end;
            }
        }

        /* Page-align the start */
        cand_start = (cand_start + 0xFFF) & ~0xFFF;

        if (cand_end > cand_start && (cand_end - cand_start) >= size)
            return cand_start;

        i++;
    }
}

/*
 * Load PT_LOAD segments from an ELF file buffer into memory at
 * load_base.  Copies file data and zeroes .bss regions.
 */
static void elf_load_segments(const unsigned char *file,
                              const struct elf32_ehdr *ehdr,
                              uint32_t load_base,
                              uint32_t base_vaddr) {
    int i;
    for (i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph =
            (const struct elf32_phdr *)(file + ehdr->e_phoff +
                                        i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;

        uint32_t dst = load_base + (ph->p_vaddr - base_vaddr);
        /* Copy file contents */
        memcpy((void *)dst, file + ph->p_offset, ph->p_filesz);
        /* Zero .bss (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(dst + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
    }
}

static void cmd_boot(const char *args) {
    const char *p = args;

    if (!(p[0] == 's' && p[1] == 'd' && p[2] == ':')) {
        console_puts("usage: boot sd:<dev>,<cs>[/file]\r\n");
        return;
    }
    p += 3;
    unsigned long dev_nth = strtoul(p, (char **)&p, 10);
    if (*p == ',') p++;
    unsigned long cs = strtoul(p, (char **)&p, 10);

    /* Optional /filename — default to PENBOOT.ELF */
    const char *filename = BOOT_FILENAME;
    if (*p == '/') {
        p++;
        if (*p == '\0') {
            console_puts("missing filename\r\n");
            return;
        }
        filename = p;
    }

    struct btag_device *dev =
        bd_find_device_by_class(ACFG_CLASS_SD, (int)dev_nth);
    if (!dev) {
        console_printf("sd:%d — no such SD controller\r\n", (int)dev_nth);
        return;
    }

    (void)cs;

    /* Init SD card */
    int rc = sd_init(dev->base);
    if (rc != 0) {
        console_printf("SD init failed (err=%d)\r\n", rc);
        return;
    }

    /* Find first FAT32 partition from MBR */
    unsigned char mbr[512];
    if (sd_read_sector(dev->base, 0, mbr) != 0) {
        console_puts("MBR read failed\r\n");
        goto out;
    }
    if (read_le16(mbr + MBR_SIG_OFFSET) != MBR_SIGNATURE) {
        console_puts("No MBR signature\r\n");
        goto out;
    }

    uint32_t part_lba = 0;
    int part;
    for (part = 0; part < 4; part++) {
        unsigned char *pe = mbr + MBR_PART_OFFSET + part * MBR_PART_ENTRY_SIZE;
        unsigned char ptype = pe[4];
        if (ptype == PTYPE_FAT32 || ptype == PTYPE_FAT32L) {
            part_lba = read_le32(pe + 8);
            break;
        }
    }
    if (part_lba == 0) {
        console_puts("No FAT32 partition found\r\n");
        goto out;
    }
    console_printf("FAT32 partition %d at LBA %d\r\n", part + 1,
                    (int)part_lba);

    /* Mount FAT32 */
    struct fat32 fs;
    rc = fat32_mount(&fs, sd_blk_read, (void *)(unsigned long)dev->base,
                     part_lba);
    if (rc != 0) {
        console_printf("FAT32 mount failed (err=%d)\r\n", rc);
        goto out;
    }

    /* Find loader file */
    uint32_t file_cluster, file_size;
    rc = fat32_find_root(&fs, filename, &file_cluster, &file_size);
    if (rc != 0) {
        console_printf("%s not found\r\n", filename);
        goto out;
    }
    console_printf("Loading %s (%d bytes)\r\n", filename,
                    (int)file_size);

    /* Base reserved region: pages 0–1 (vectors, boot data, stack) */
    struct reserved_region res_low = { 0x00000000, 4096 * 2 };
    struct reserved_region *res_base[] = { &res_low, 0, 0, 0 };

    /* Allocate scratch buffer for the raw ELF file */
    uint32_t scratch_addr = find_memory_region(file_size, res_base);
    if (scratch_addr == 0) {
        console_puts("No RAM for scratch buffer\r\n");
        goto out;
    }

    unsigned char *scratch = (unsigned char *)scratch_addr;
    rc = fat32_read_file(&fs, file_cluster, scratch, file_size);
    if (rc != 0) {
        console_puts("Read error\r\n");
        goto out;
    }

    sd_deinit(dev->base);

    /* Parse and validate ELF */
    const struct elf32_ehdr *ehdr = (const struct elf32_ehdr *)scratch;
    if (elf_validate(ehdr) != 0)
        return;

    /* Compute memory footprint */
    uint32_t base_vaddr;
    uint32_t memsz = elf_memsz(scratch, ehdr, &base_vaddr);
    console_printf("ELF: %d bytes in memory, vaddr base 0x%x\r\n",
                    (int)memsz, base_vaddr);

    /* Allocate load destination (scratch is now also reserved) */
    struct reserved_region res_scratch = { scratch_addr, file_size };
    res_base[1] = &res_scratch;
    uint32_t load_base = find_memory_region(memsz, res_base);
    if (load_base == 0) {
        console_puts("No suitable RAM region for loader\r\n");
        return;
    }
    console_printf("Loading at 0x%x\r\n", load_base);

    /* Copy PT_LOAD segments to their final location */
    elf_load_segments(scratch, ehdr, load_base, base_vaddr);

    /* Entry point adjusted for actual load position */
    uint32_t entry = load_base + (ehdr->e_entry - base_vaddr);
    console_printf("Jumping to 0x%x\r\n\r\n", entry);

    /* Jump with R1 = boot data pointer */
    uint32_t bd = BOOTDATA_BASE;
    asm volatile(
        "mov r1, %0\n\t"
        "jmp %1"
        : : "r"(bd), "r"(entry)
        : "r1"
    );
    __builtin_unreachable();

out:
    sd_deinit(dev->base);
}

/*
 * cmd_go — jump to an address and execute.
 *
 * Usage: go <addr>
 *
 * Calls the target as a C function with R1 = boot data pointer
 * (BOOTDATA_BASE). If the target returns, we fall back into the
 * monitor loop. A real stage 1 bootloader won't return.
 */
static void cmd_go(const char *args) {
    const char *p = args;
    unsigned long addr = strtoul(p, (char **)&p, 16);

    if (addr == 0) {
        console_puts("usage: go <addr>\r\n");
        return;
    }

    console_printf("jumping to 0x%x\r\n", (unsigned int)addr);

    /* Set R1 = boot data pointer, then jump.  One-way — the target
     * is not expected to return (stage 1 takes over, or test code
     * ends with BREAK). */
    uint32_t bd = BOOTDATA_BASE;
    asm volatile(
        "mov r1, %0\n\t"
        "jmp %1"
        : : "r"(bd), "r"((uint32_t)addr)
        : "r1"
    );
    __builtin_unreachable();
}

/* ── System identification ────────────────────────────────────────── */

/*
 * Unpack a 32-bit LE word into 4 bytes of a name buffer.
 */
static void unpack_name_word(char *buf, int pos, uint32_t word) {
    buf[pos + 0] = (char)(word & 0xFF);
    buf[pos + 1] = (char)((word >> 8) & 0xFF);
    buf[pos + 2] = (char)((word >> 16) & 0xFF);
    buf[pos + 3] = (char)((word >> 24) & 0xFF);
}

/*
 * Read CPU name from sysid regs 2–5 into buf (17 bytes min).
 * Each RDSYS uses a compile-time constant register number.
 */
static void read_cpu_name(char *buf) {
    unpack_name_word(buf,  0, penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_NAME0));
    unpack_name_word(buf,  4, penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_NAME1));
    unpack_name_word(buf,  8, penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_NAME2));
    unpack_name_word(buf, 12, penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_NAME3));
    buf[16] = '\0';
}

/*
 * Read machine name from sysid regs 6–9 into buf (17 bytes min).
 */
static void read_mach_name(char *buf) {
    unpack_name_word(buf,  0, penumbra_read_sysreg(SYSDEV_SYSID, SYS_MACH_NAME0));
    unpack_name_word(buf,  4, penumbra_read_sysreg(SYSDEV_SYSID, SYS_MACH_NAME1));
    unpack_name_word(buf,  8, penumbra_read_sysreg(SYSDEV_SYSID, SYS_MACH_NAME2));
    unpack_name_word(buf, 12, penumbra_read_sysreg(SYSDEV_SYSID, SYS_MACH_NAME3));
    buf[16] = '\0';
}

/*
 * Decode CPU_ISA register value into a human-readable string.
 * isa_val has ISA version in bits [3:0] and feature flags in [31:4].
 *
 * Example outputs:
 *   "ISA v1"                    — base ISA, no optional features
 *   "ISA v1, MUL"               — hardware multiply
 *   "ISA v1, MUL, DIV, FPU"    — all features
 *   "ISA v1, MUL, UNK_3"       — unknown future feature bit
 */
static void format_cpu_features(char *buf, int bufsz, uint32_t isa_val) {
    int version = isa_val & 0x0F;
    snprintf(buf, bufsz, "ISA v%d", version);

    for (int bit = 0; bit < 28; bit++) {
        /* Feature flags start at bit 4 */
        uint32_t mask = 1u << (bit + 4);
        /* TODO: flag meanings could differ per ISA version */
        if (isa_val & mask) {
            switch (bit) {
            case CPU_FEAT_BIT_HW_MUL:
                strncat(buf, ", MUL", bufsz);
                break;
            case CPU_FEAT_BIT_HW_DIV:
                strncat(buf, ", DIV", bufsz);
                break;
            case CPU_FEAT_BIT_FPU:
                strncat(buf, ", FPU", bufsz);
                break;
            default: {
                char tmp[12];
                snprintf(tmp, sizeof(tmp), ", UNK_%d", bit);
                strncat(buf, tmp, bufsz);
                break;
            }
            }
        }
    }
}

static void print_banner(void) {
    char cpu_name[17], mach_name[17], feat_str[48];

    read_cpu_name(cpu_name);
    read_mach_name(mach_name);

    uint32_t cpu_isa = penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_ISA);
    format_cpu_features(feat_str, sizeof(feat_str), cpu_isa);

    uint32_t cpu_freq = penumbra_read_sysreg(SYSDEV_SYSID, SYS_CPU_FREQ);

    console_puts("\r\nPenumbra boot\r\n\r\n");
    console_printf("CPU:      %s (%s)", cpu_name, feat_str);
    if (cpu_freq > 0) {
        uint32_t mhz = cpu_freq / 1000000;
        uint32_t khz_frac = (cpu_freq / 1000) % 1000;
        if (khz_frac)
            console_printf(" @ %d.%03d MHz", (int)mhz, (int)khz_frac);
        else
            console_printf(" @ %d MHz", (int)mhz);
    }
    console_puts("\r\n");
    console_printf("Hardware: %s\r\n\r\n", mach_name);
}

/* ── Main and boot sequence ───────────────────────────────────────── */

int main(void) {
    char cmdbuffer[64];

    print_banner();

    setup_traps();

    /* ── Initialize boot data tagged list ──────────────────────── */
    /* Zero the boot data area — SDRAM contains random garbage at
     * power-up (unlike BRAM which is bitstream-initialized to 0).
     * The tagged list walkers rely on BTAG_END (0) as a sentinel,
     * so any unwritten memory must be zero.  One page is plenty. */
    memset((void *)BOOTDATA_BASE, 0, 4096 - BOOTDATA_BASE);
    uint32_t bd_cursor = bd_init();

    /* ── RAM detection ─────────────────────────────────────────── */
    console_puts("Detecting base RAM... ");
    long npages = detect_ram();
    char ram_size_str[32];
    humanize_size((unsigned long)npages * 4096, ram_size_str, sizeof(ram_size_str));
    console_printf("%s found\r\n", ram_size_str);
    bd_add_device(&bd_cursor, ACFG_CLASS_MEMORY, 0x00000000,
                  (uint32_t)(npages * 4096), 0, "RAM");

    /* ── Inject built-in UART as a device ──────────────────────── */
    int uart_dev = bd_add_device(&bd_cursor, ACFG_CLASS_UART,
                                 UART_ADDR, 4096, 0, "UART");
    bd_add_console(&bd_cursor, uart_dev);

    /* ── Bus autoconfig ────────────────────────────────────────── */
    console_puts("Probing bus devices...\r\n");
    int ndevs = autoconfig(&bd_cursor);
    console_printf("%d device(s) found\r\n", ndevs);

    /* ── SD card presence check ───────────────────────────────── */
    console_puts("Probing SD slots...\r\n");
    int boot_sd = sd_probe();    /* SD controller index, not global */
    if (boot_sd >= 0) {
        struct btag_device *sd_dev =
            bd_find_device_by_class(ACFG_CLASS_SD, boot_sd);
        bd_add_bootdev(&bd_cursor, bd_device_index(sd_dev), 0, 0);
    }

    /* ── Finalize boot data ────────────────────────────────────── */
    bd_finalize(&bd_cursor);
    console_printf("Boot data: %d bytes at 0x%x\r\n\r\n",
                    (int)(bd_cursor - BOOTDATA_BASE),
                    BOOTDATA_BASE);

    /* Monitor command loop */
    for (;;) {
        console_puts("> ");

        int len = console_gets(cmdbuffer, sizeof(cmdbuffer));
        if (len > 0) {
            if (strcmp(cmdbuffer, "b") == 0 ||
                strcmp(cmdbuffer, "break") == 0) {
                asm volatile("break");
            } else if (cmdbuffer[0] == 'x' &&
                       (cmdbuffer[1] == ' ' || cmdbuffer[1] == '\0')) {
                cmd_examine(cmdbuffer + 1);
            } else if (strncmp(cmdbuffer, "examine ", 8) == 0) {
                cmd_examine(cmdbuffer + 8);
            } else if (strncmp(cmdbuffer, "load ", 5) == 0) {
                cmd_load(cmdbuffer + 5);
            } else if (strncmp(cmdbuffer, "part ", 5) == 0) {
                cmd_part(cmdbuffer + 5);
            } else if (cmdbuffer[0] == 'g' &&
                       (cmdbuffer[1] == ' ' || cmdbuffer[1] == '\0')) {
                cmd_go(cmdbuffer + 1);
            } else if (strncmp(cmdbuffer, "go ", 3) == 0) {
                cmd_go(cmdbuffer + 3);
            } else if (strncmp(cmdbuffer, "boot ", 5) == 0) {
                cmd_boot(cmdbuffer + 5);
            } else {
                console_puts("?\r\n");
            }
        }
    }
}

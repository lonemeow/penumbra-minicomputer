/*
 * boot_rom.c — Penumbra boot ROM
 *
 * Boot sequence: traps → RAM detect → inject UART device → autoconfig
 * → SD card probe → monitor loop. All discovered state lives in the
 * boot data tagged list at BOOTDATA_BASE (page 0, after vectors).
 * No globals — ROM has no writable data section.
 */

#include "uart.h"
#include "spi.h"
#include "bootdata.h"
#include "libc.h"
#include "penumbra.h"
#include <stdarg.h>

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

static void console_putc(char c) {
    uart_write(c);
}

static void console_puts(const char *s) {
    while (*s) {
        uart_write(*s++);
    }
}

/*
 * console_gets — read a line with editing into buffer (max chars incl. NUL).
 *
 * Supports:
 *   - Printable characters (0x20–0x7E): insert at cursor
 *   - Backspace (0x08) or DEL (0x7F): delete character before cursor
 *   - Ctrl-U (0x15): kill entire line
 *   - Enter (\r or \n): accept line
 *
 * Echoes characters as typed. Backspace sends "\b \b" (back, space, back)
 * to erase the character on the terminal.
 *
 * Returns the length of the entered string (not counting NUL).
 */
static int console_gets(char *buffer, int max) {
    int i = 0;

    for (;;) {
        int c = uart_read();

        if (c == '\r' || c == '\n') {
            console_puts("\r\n");
            break;
        }

        switch (c) {
            case 0x08:
            case 0x7F:
                if (i > 0) {
                    i--;
                    console_puts("\b \b");
                }
                break;
            case 0x15:
                for (; i>0; i--) {
                    console_puts("\b \b");
                }
                break;

            default:
                if (isprint(c) && i < (max - 1)) {
                    buffer[i++] = (char)c;
                    console_putc(c);
                }
        }
    }

    buffer[i] = '\0';
    return i;
}

/* Formatted output helpers. */
static void console_put_unsigned(unsigned int val, int base) {
    char buf[12];
    console_puts(utoa(val, buf, base));
}

static void console_put_dec(int val) {
    if (val < 0) {
        console_putc('-');
        val = -val;
    }
    console_put_unsigned((unsigned int)val, 10);
}

static void console_put_hex(unsigned int val) {
    console_puts("0x");
    console_put_unsigned(val, 16);
}

static void console_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_puts(buf);
}

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

    /* Brief delay for async bus devices */
    for (volatile int i = 0; i < 100; i++) {}

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

/* ── SD card (SPI mode) ───────────────────────────────────────────── *
 *
 * Fully stateless: each operation does init → work → deinit, so the
 * card can be swapped between commands and the next boot stage
 * inherits clean hardware state.
 */

/* R1 response bit masks */
#define SD_R1_IDLE       0x01
#define SD_R1_ERASE_RST  0x02
#define SD_R1_ILLEGAL    0x04
#define SD_R1_CRC_ERR    0x08
#define SD_R1_ERASE_SEQ  0x10
#define SD_R1_ADDR_ERR   0x20
#define SD_R1_PARAM_ERR  0x40
#define SD_R1_NO_RESP    0xFF

/* Timeout limits (iteration counts, not real time) */
#define SD_CMD0_RETRIES   20
#define SD_ACMD41_RETRIES 1500
#define SD_DATA_RETRIES   1000
#define SD_RESP_RETRIES   8

/*
 * Send an SD command (6 bytes) and return the R1 response.
 * Polls up to SD_RESP_RETRIES bytes waiting for bit 7 to clear.
 */
static unsigned char sd_command(uint32_t base, unsigned char cmd,
                                uint32_t arg) {
    spi_transfer(base, 0x40 | cmd);
    spi_transfer(base, (unsigned char)(arg >> 24));
    spi_transfer(base, (unsigned char)(arg >> 16));
    spi_transfer(base, (unsigned char)(arg >> 8));
    spi_transfer(base, (unsigned char)(arg));
    /* CRC — only CMD0 and CMD8 need valid CRC in SPI mode */
    if (cmd == 0)
        spi_transfer(base, 0x95);
    else if (cmd == 8)
        spi_transfer(base, 0x87);
    else
        spi_transfer(base, 0xFF);

    unsigned char r;
    for (int i = 0; i < SD_RESP_RETRIES; i++) {
        r = spi_transfer(base, 0xFF);
        if (!(r & 0x80))
            return r;
    }
    return r;
}

/* Read a 32-bit big-endian response (CMD8 R7 tail, CMD58 OCR, etc.) */
static uint32_t sd_read_response32(uint32_t base) {
    uint32_t val;
    val  = (uint32_t)spi_transfer(base, 0xFF) << 24;
    val |= (uint32_t)spi_transfer(base, 0xFF) << 16;
    val |= (uint32_t)spi_transfer(base, 0xFF) << 8;
    val |= (uint32_t)spi_transfer(base, 0xFF);
    return val;
}

/*
 * Initialize SD card on the given SPI controller.
 * Leaves CS asserted and clock set fast on success.
 * On failure, deasserts CS and resets clock.
 *
 * Returns 0 on success, negative error code on failure:
 *   -1  CMD0 no response (empty slot)
 *   -2  CMD0 unexpected response
 *   -3  CMD8 rejected
 *   -4  CMD8 voltage/pattern mismatch
 *   -5  CMD55 rejected
 *   -6  ACMD41 rejected
 *   -7  ACMD41 timeout (card never left idle)
 *   -8  CMD58 not SDHC (no block addressing)
 */
static int sd_init(uint32_t base) {
    unsigned char r1;

    /* Slow clock for init (≤400 kHz) */
    spi_set_clkdiv(base, 0xFF);

    /* 80+ clock cycles with CS deasserted (card power-up) */
    spi_cs0(base, 1);
    for (int i = 0; i < 20; i++)
        spi_transfer(base, 0xFF);

    spi_cs0(base, 0);

    /* CMD0 — go idle */
    for (int i = 0; i < SD_CMD0_RETRIES; i++) {
        r1 = sd_command(base, 0, 0);
        if (r1 == SD_R1_IDLE)
            break;
    }
    if (r1 == SD_R1_NO_RESP) { r1 = -1; goto fail; }
    if (r1 != SD_R1_IDLE)    { r1 = -2; goto fail; }

    /* CMD8 — send interface condition (voltage check) */
    r1 = sd_command(base, 8, 0x1AA);
    if (r1 != SD_R1_IDLE) { r1 = -3; goto fail; }
    uint32_t r7 = sd_read_response32(base);
    if ((r7 & 0xFFF) != 0x1AA) { r1 = -4; goto fail; }

    /* CMD55 + ACMD41 — app-specific init, wait for ready */
    for (int i = 0; i < SD_ACMD41_RETRIES; i++) {
        r1 = sd_command(base, 55, 0);
        if (r1 != SD_R1_IDLE) { r1 = -5; goto fail; }
        r1 = sd_command(base, 41, 0x40000000);
        if (r1 == 0x00)
            break;
        if (r1 != SD_R1_IDLE) { r1 = -6; goto fail; }
    }
    if (r1 != 0x00) { r1 = -7; goto fail; }

    /* CMD58 — read OCR, check SDHC (block addressing) */
    r1 = sd_command(base, 58, 0);
    uint32_t ocr = sd_read_response32(base);
    if (!(ocr & 0x40000000)) { r1 = -8; goto fail; }

    /* Switch to fast clock for data transfers */
    spi_set_clkdiv(base, 0);
    return 0;

fail:
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);
    return (int)(signed char)r1;
}

/*
 * Return the SPI controller to clean state: CS deasserted, slow
 * clock. The next user (ROM monitor retry, stage 1 bootloader)
 * starts from a known baseline.
 */
static void sd_deinit(uint32_t base) {
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);
}

/*
 * Read one 512-byte sector from an already-initialized SD card.
 * Returns 0 on success, -1 on error.
 */
static int sd_read_sector(uint32_t base, uint32_t lba,
                          unsigned char *dst) {
    unsigned char r1 = sd_command(base, 17, lba);
    if (r1 != 0x00)
        return -1;

    /* Wait for data token (0xFE) */
    unsigned char tok;
    for (int i = 0; i < SD_DATA_RETRIES; i++) {
        tok = spi_transfer(base, 0xFF);
        if (tok == 0xFE)
            break;
    }
    if (tok != 0xFE)
        return -1;

    for (int i = 0; i < 512; i++)
        dst[i] = spi_transfer(base, 0xFF);

    /* Discard CRC16 */
    spi_transfer(base, 0xFF);
    spi_transfer(base, 0xFF);

    return 0;
}

/*
 * Probe for SD card presence on a single controller.
 * Does CMD0 only — just checks if a card responds.
 * Returns 1 if a card is present, 0 if empty slot.
 */
static int sd_detect(uint32_t base) {
    unsigned char r1;

    spi_set_clkdiv(base, 0xFF);
    spi_cs0(base, 1);
    for (int i = 0; i < 20; i++)
        spi_transfer(base, 0xFF);

    spi_cs0(base, 0);
    for (int i = 0; i < SD_CMD0_RETRIES; i++) {
        r1 = sd_command(base, 0, 0);
        if (r1 == SD_R1_IDLE)
            break;
    }
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);

    return r1 == SD_R1_IDLE;
}

/*
 * Probe all CLASS_SD devices and report which have cards.
 * Returns the device index of the first SD card found, or -1.
 */
static int sd_probe(void) {
    int first = -1;
    for (int i = 0; ; i++) {
        struct btag_device *dev = bd_find_device_by_class(ACFG_CLASS_SD, i);
        if (!dev)
            break;
        int idx = bd_device_index(dev);
        console_printf("  sd:%d,0 ... ", idx);
        if (sd_detect(dev->base)) {
            console_puts("card present\r\n");
            if (first < 0)
                first = idx;
        } else {
            console_puts("empty\r\n");
        }
    }
    return first;
}

/*
 * cmd_load — read sectors from SD card into memory.
 *
 * Usage: load sd:<dev>,<cs> <addr> <lba> <count>
 *
 * Full lifecycle per command: init → read sectors → deinit.
 * Card can be swapped between load commands.
 */
static void cmd_load(const char *args) {
    const char *p = args;

    /* Parse sd:<dev>,<cs> */
    if (!(p[0] == 's' && p[1] == 'd' && p[2] == ':')) {
        console_puts("usage: load sd:<dev>,<cs> <addr> <lba> <count>\r\n");
        return;
    }
    p += 3;
    unsigned long dev_nth = strtoul(p, (char **)&p, 10);
    if (*p == ',') p++;
    unsigned long cs = strtoul(p, (char **)&p, 10);
    while (*p == ' ') p++;

    unsigned long addr  = strtoul(p, (char **)&p, 16);
    unsigned long lba   = strtoul(p, (char **)&p, 10);
    unsigned long count = strtoul(p, (char **)&p, 10);

    if (count == 0) {
        console_puts("usage: load sd:<dev>,<cs> <addr> <lba> <count>\r\n");
        return;
    }

    struct btag_device *dev = bd_find_device((int)dev_nth);
    if (!dev || dev->cls != ACFG_CLASS_SD) {
        console_printf("sd:%d — not an SD device\r\n", (int)dev_nth);
        return;
    }

    (void)cs;  /* TODO: support CS1 */

    /* Init card fresh each time (supports hot-swap) */
    int rc = sd_init(dev->base);
    if (rc != 0) {
        console_printf("SD init failed (err=%d)\r\n", rc);
        return;
    }

    unsigned char *dst = (unsigned char *)addr;
    int err = 0;
    for (unsigned long i = 0; i < count; i++) {
        if (sd_read_sector(dev->base, lba + i, dst) != 0) {
            console_printf("read error at LBA %d\r\n", (int)(lba + i));
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

int main(void) {
    char cmdbuffer[64];

    console_puts("Penumbra/1 boot\r\n\r\n");

    setup_traps();

    /* ── Initialize boot data tagged list ──────────────────────── */
    uint32_t bd_cursor = bd_init();

    /* ── RAM detection ─────────────────────────────────────────── */
    console_puts("Detecting base RAM... ");
    long npages = detect_ram();
    long ram_kb = npages * 4096 / 1024;
    console_printf("%dkB found\r\n", (int)ram_kb);
    bd_add_memory(&bd_cursor, 0x00000000, (uint32_t)(npages * 4096));

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
    int boot_sd = sd_probe();
    if (boot_sd >= 0)
        bd_add_bootdev(&bd_cursor, boot_sd, 0, 0);

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
            } else {
                console_puts("?\r\n");
            }
        }
    }
}

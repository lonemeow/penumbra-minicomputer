/*
 * boot.c — Penumbra boot loader
 *
 * Loaded by the ROM as PENBOOT.ELF (PIE) from FAT32.
 * Receives R1 = physical pointer to Penumbra boot data.
 *
 * Walks the firmware boot data in a single pass to:
 *   1. Build a NetBSD bootinfo structure
 *   2. Collect physical memory regions
 *
 * Then loads the kernel ELF at a dynamically chosen physical
 * address, populates remaining bootinfo entries (symtab, kernbase),
 * and jumps to the kernel's physical entry with MMU off.
 * The kernel's locore.S PIC stub handles MMU setup.
 */

#include <lib/libsa/stand.h>
#include <lib/libsa/loadfile.h>
#include <lib/libsa/bootcfg.h>
#include <lib/libkern/libkern.h>
#include <machine/bootinfo.h>
#include "bootdata_defs.h"

/* SD block device init (from libsa/sdblk.c) */
extern int sd_boot_init(uint32_t bootdata);

/* Forward declarations for translation helpers */
static struct btag_device *find_device_nth(uint32_t, int);
static int translate_bootdata_device(struct btag_device *);
static int translate_bootdata_bootdev(struct btag_bootdev *, uint32_t);
static int translate_bootdata_console(struct btag_console *, uint32_t);

/* Kernel filename on the FAT32 boot partition */
static const char *kernel_paths[] = {
	"netbsd",
	"netbsd.gz",
	NULL
};

/* ── Memory region tracking ──────────────────────────────────────── */

#define MAX_MEMREGIONS	8
#define MAX_RESERVED	4

struct memregion {
	uint32_t base;
	uint32_t size;
};

static struct memregion memregions[MAX_MEMREGIONS];
static int nmemregions;

/*
 * Find a contiguous region of `size` bytes in the collected memory map
 * (memregions[]), avoiding all reserved regions in `res[]`.
 *
 * Parameters:
 *   size  — minimum bytes needed
 *   res   — array of reserved regions to avoid
 *   nres  — number of entries in res[]
 *
 * Returns a page-aligned (4 KB) physical address, or 0 on failure.
 */
static uint32_t
find_free_region(uint32_t size, const struct memregion *res, int nres)
{
	for (int i = 0; i < nmemregions; i++)
	{
		uint32_t cand_start = memregions[i].base;
		uint32_t cand_end = cand_start + memregions[i].size;

		for (int j = 0; j < nres; j++)
		{
			uint32_t res_start = res[j].base;
			uint32_t res_end = res_start + res[j].size;

			if (res_start <= cand_start && res_end >= cand_end)
			{
				/* Completely covered */
				cand_start = cand_end;
			}
			else if (res_start <= cand_start && res_end > cand_start)
			{
				/* Overlaps at start */
				cand_start = res_end;
			}
			else if (res_start < cand_end && res_end >= cand_end)
			{
				/* Overlaps at end */
				cand_end = res_start;
			}
			else if (res_start > cand_start && res_end < cand_end)
			{
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
	}

	return 0;
}

/* ── Bootinfo builder ────────────────────────────────────────────── */

/* Bootinfo is built into this static buffer */
static char bootinfo_buf[BOOTINFO_MAXSIZE] __attribute__((aligned(4)));
static struct bootinfo *bi = (struct bootinfo *)bootinfo_buf;
static char *bi_next;		/* next free byte in bootinfo_buf */

static void
bi_init(void)
{
	bi->magic = BOOTINFO_MAGIC;
	bi->size = sizeof(struct bootinfo);
	bi->nentries = 0;
	bi_next = bootinfo_buf + sizeof(struct bootinfo);
}

static void *
bi_alloc(uint32_t type, uint32_t len)
{
	struct btinfo_common *entry;

	/* Ensure 4-byte alignment */
	len = (len + 3) & ~3;

	if ((bi_next - bootinfo_buf) + len > BOOTINFO_MAXSIZE) {
		printf("bootinfo overflow\n");
		return NULL;
	}

	entry = (struct btinfo_common *)bi_next;
	entry->len = len;
	entry->type = type;
	bi->nentries++;
	bi->size += len;
	bi_next += len;
	return entry;
}

/* ── Boot data → bootinfo translation ────────────────────────────── */

/*
 * translate_bootdata — walk firmware boot data, build bootinfo
 *
 * Single pass over the Penumbra boot data tagged list.
 * For each entry, emit the corresponding bootinfo entry and
 * collect memory regions into memregions[].
 */
static int
translate_bootdata(uint32_t bootdata)
{
	struct bootdata_hdr *hdr = (struct bootdata_hdr *)bootdata;
	uint32_t p;

	if (hdr->magic != BOOTDATA_MAGIC) {
		printf("Bad boot data magic: 0x%x\n", hdr->magic);
		return -1;
	}

	bi_init();
	nmemregions = 0;
	p = bootdata + sizeof(struct bootdata_hdr);

	while (1)
	{
		struct btag_hdr *h = (struct btag_hdr *)p;
		p += h->size;

		switch (h->type)
		{
		case BTAG_END:
			return 0;

		case BTAG_DEVICE:
			if (translate_bootdata_device((struct btag_device *)h) != 0)
				return -1;
			break;

		case BTAG_BOOTDEV:
			if (translate_bootdata_bootdev((struct btag_bootdev *)h, bootdata) != 0)
				return -1;
			break;

		case BTAG_CONSOLE:
			if (translate_bootdata_console((struct btag_console *)h, bootdata) != 0)
				return -1;
			break;

		default:
			break;
		}
	}
}

static int
translate_bootdata_console(struct btag_console *cdev, uint32_t bootdata)
{
	struct btag_device *dev = find_device_nth(bootdata, cdev->dev_nth);
	if (dev == NULL)
	{
		printf("Boot device not found in device table");
		return -1;
	}

	struct btinfo_console *info = bi_alloc(BTINFO_CONSOLE, sizeof(struct btinfo_console));
	if (info == NULL)
		return -1;

	info->addr = dev->base;
	strcpy(info->devname, "com0");

	return 0;
}

static int
translate_bootdata_bootdev(struct btag_bootdev *bdev, uint32_t bootdata)
{
	struct btag_device *dev = find_device_nth(bootdata, bdev->dev_nth);
	if (dev == NULL)
	{
		printf("Boot device not found in device table");
		return -1;
	}

	struct btinfo_bootpath *info = bi_alloc(BTINFO_BOOTPATH, sizeof(struct btinfo_bootpath));
	if (info == NULL)
		return -1;

	info->bus_addr = dev->base;
	// FIXME: Poor naming, this should be just device index (cs is SDcard specific terminology)
	info->cs = bdev->cs;
	info->partition = bdev->partition;

	return 0;
}

static int
translate_bootdata_device(struct btag_device *dev)
{
	switch (dev->cls)
	{
	case ACFG_CLASS_MEMORY:
	{
		if (nmemregions < MAX_MEMREGIONS)
		{
			memregions[nmemregions].base = dev->base;
			memregions[nmemregions].size = dev->dev_size;
			nmemregions++;
		}
		struct btinfo_memory *info = bi_alloc(BTINFO_MEMORY, sizeof(struct btinfo_memory));
		if (info == NULL)
			return -1;

		info->base = dev->base;
		info->size = dev->dev_size;
		break;
	}
	default:
	{
		/* Pass through all other devices (UART, SPI, etc.)
		 * — kernel can't re-run autoconfig without bus reset. */
		struct btinfo_device *info = bi_alloc(BTINFO_DEVICE,
		    sizeof(struct btinfo_device));
		if (info == NULL)
			return -1;
		info->cls = dev->cls;
		info->addr = dev->base;
		info->size = dev->dev_size;
		info->id = dev->id;
		memcpy(info->name, dev->name, sizeof(info->name));
		break;
	}
	}

	return 0;
}

/*
 * Find the nth BTAG_DEVICE entry in boot data.
 */
static struct btag_device *
find_device_nth(uint32_t bootdata, int nth)
{
	uint32_t q = bootdata + sizeof(struct bootdata_hdr);
	int count = 0;

	for (;;) {
		struct btag_hdr *h = (struct btag_hdr *)q;
		if (h->type == BTAG_END)
			return NULL;
		if (h->type == BTAG_DEVICE) {
			if (count == nth)
				return (struct btag_device *)q;
			count++;
		}
		q += h->size;
	}
}

/* ── devopen / _rtt (required by libsa) ──────────────────────────── */

int
devopen(struct open_file *f, const char *fname, char **file)
{
	f->f_dev = &devsw[0];
	*file = (char *)fname;
	return 0;
}

void
_rtt(void)
{
	printf("Halting.\n");
	__asm volatile("break");
	for (;;)
		;
}

/* ── Boot config file ───────────────────────────────────────────── */

/*
 * Read boot.cfg from the FAT32 boot partition via libsa's
 * perform_bootcfg() and emit bootinfo entries for recognized keys.
 *
 * Currently used:
 *   root=ld0f   → BTINFO_ROOTDEVICE (auto-select root device)
 */
static void
load_boot_config(void)
{
	int rc;

	rc = perform_bootcfg(BOOTCFG_FILENAME, bootcfg_do_noop, 0);
	if (rc != 0)
		return;		/* no boot.cfg — that's fine */

	if (bootcfg_info.root != NULL) {
		struct btinfo_rootdevice *bi_root;

		bi_root = bi_alloc(BTINFO_ROOTDEVICE,
		    sizeof(struct btinfo_rootdevice));
		if (bi_root != NULL) {
			strlcpy(bi_root->devname, bootcfg_info.root,
			    sizeof(bi_root->devname));
			printf("Root device: %s (from boot.cfg)\n",
			    bi_root->devname);
		}
	}
}

/* ── Main ────────────────────────────────────────────────────────── */

#define KERNEL_TEXT_BASE	0x80010000
#define HEAP_SIZE		(64 * 1024)

/* Linker-provided symbols for the bootloader's own footprint */
extern char _start[], _end[];

int
main(uint32_t bootdata)
{
	const char **path;
	u_long marks[MARK_MAX];
	u_long kernel_size, offset;
	uint32_t load_phys;
	struct btinfo_symtab *bi_sym;
	struct btinfo_kernbase *bi_kern;
	int rc;

	printf("NetBSD/penumbra boot\n\n");

	/* Phase 1: Translate boot data → bootinfo + memory map */
	rc = translate_bootdata(bootdata);
	if (rc != 0)
		_rtt();

	printf("Memory regions: %d\n", nmemregions);
	for (rc = 0; rc < nmemregions; rc++)
		printf("  0x%x - 0x%x (%u KB)\n",
		    memregions[rc].base,
		    memregions[rc].base + memregions[rc].size,
		    memregions[rc].size / 1024);

	/*
	 * Phase 1.5: Set up heap in a safe RAM region.
	 *
	 * The bootloader is a PIE loaded at an arbitrary address.
	 * Reserve the low page (vectors + boot data + stack) and the
	 * bootloader's own text/data/bss, then find a free region
	 * for the libsa heap.
	 */
	struct memregion res[MAX_RESERVED];
	int nres = 0;

	/* Low memory: vectors (page 0), boot data, CRT stack (sp = 0xB000) */
	res[nres].base = 0;
	res[nres].size = 0xC000;
	nres++;

	/* Bootloader's own PIE image */
	res[nres].base = (uint32_t)_start;
	res[nres].size = (uint32_t)(_end - _start);
	nres++;

	uint32_t heap_base = find_free_region(HEAP_SIZE, res, nres);
	if (heap_base == 0) {
		printf("No RAM for heap.\n");
		_rtt();
	}
	setheap((void *)heap_base, (void *)(heap_base + HEAP_SIZE));
	printf("Heap: 0x%x - 0x%x (%u KB)\n",
	    heap_base, heap_base + HEAP_SIZE, HEAP_SIZE / 1024);

	/* Add heap to reserved list for subsequent allocations */
	res[nres].base = heap_base;
	res[nres].size = HEAP_SIZE;
	nres++;

	/* Phase 2: Init SD and find the kernel on FAT32 */
	if (sd_boot_init(bootdata) != 0) {
		printf("SD init failed.\n");
		_rtt();
	}

	/* Phase 2.5: Read boot config file for root device etc. */
	load_boot_config();

	/* Phase 3: COUNT pass — measure kernel memory footprint.
	 * loadfile() returns fd on success, -1 on failure. */
	for (path = kernel_paths; *path != NULL; path++) {
		marks[MARK_START] = 0;
		rc = loadfile(*path, marks, COUNT_KERNEL);
		if (rc >= 0) {
			close(rc);
			printf("Found %s\n", *path);
			break;
		}
	}
	if (*path == NULL) {
		printf("Kernel not found.\n");
		_rtt();
	}

	kernel_size = marks[MARK_END] - marks[MARK_START];
	printf("Kernel: %lu bytes, vaddr 0x%lx - 0x%lx\n",
	    kernel_size, marks[MARK_START], marks[MARK_END]);

	/* Phase 4: Allocate physical RAM for the kernel,
	 * avoiding low mem, bootloader, and heap. */
	load_phys = find_free_region(kernel_size, res, nres);
	if (load_phys == 0) {
		printf("No RAM for kernel.\n");
		_rtt();
	}
	printf("Loading at physical 0x%x\n", load_phys);

	/*
	 * Compute loadfile offset:
	 * The kernel is linked at KERNEL_TEXT_BASE.  loadfile() adds
	 * marks[MARK_START] to every p_vaddr.  We want:
	 *   p_vaddr + offset == physical load address
	 * So: offset = load_phys - KERNEL_TEXT_BASE
	 * (wraps unsigned — that's fine, the addition wraps back)
	 */
	offset = (u_long)load_phys - (u_long)KERNEL_TEXT_BASE;

	/* Phase 5: LOAD pass — actually read kernel into RAM */
	marks[MARK_START] = offset;
	rc = loadfile(*path, marks, LOAD_KERNEL);
	if (rc < 0) {
		printf("Load failed (errno=%d: %s).\n",
		    errno, strerror(errno));
		_rtt();
	}
	close(rc);

	/*
	 * marks[] are physical addresses after the LOAD pass —
	 * loadfile() applied offset via LOADADDR().
	 * Convert back to virtual for bootinfo entries that the
	 * kernel expects as virtual addresses.
	 */
	printf("Entry: 0x%lx (phys)\n", marks[MARK_ENTRY]);

	/* Phase 6: Add final bootinfo entries */
	bi_sym = bi_alloc(BTINFO_SYMTAB, sizeof(*bi_sym));
	if (bi_sym != NULL) {
		bi_sym->nsym = marks[MARK_NSYM];
		bi_sym->ssym = marks[MARK_SYM] - offset;
		bi_sym->esym = marks[MARK_END] - offset;
	}

	bi_kern = bi_alloc(BTINFO_KERNBASE, sizeof(*bi_kern));
	if (bi_kern != NULL) {
		bi_kern->phys_base = load_phys;
		bi_kern->kern_start = marks[MARK_START] - offset;
		bi_kern->kern_end = marks[MARK_END] - offset;
	}

	printf("Bootinfo: %d entries, %d bytes\n",
	    bi->nentries, (int)(bi_next - bootinfo_buf));

	/* Phase 7: Jump to kernel with MMU off.
	 * R1 = bootinfo (physical), R2 = kernel phys entry point.
	 * The kernel's locore.S PIC stub takes it from here. */
	uint32_t bi_phys = (uint32_t)bootinfo_buf;
	uint32_t entry_phys = (uint32_t)marks[MARK_ENTRY];

	printf("Jumping to kernel at 0x%x\n\n", entry_phys);

	__asm volatile(
	    "mov r1, %0\n\t"
	    "mov r2, %1\n\t"
	    "jmp %1"
	    : : "r"(bi_phys), "r"(entry_phys)
	    : "r1", "r2"
	);
	__builtin_unreachable();
}

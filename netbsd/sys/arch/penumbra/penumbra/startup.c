/*	$NetBSD$	*/

/*
 * Penumbra early boot initialization.
 *
 * This file contains everything needed to get from locore.S handoff
 * to main(): bootstrap TLB region table, bootinfo parsing, early
 * console, physical memory registration, and the penumbra_init()
 * entry point itself.
 *
 * Nothing here should be called after pmap installs the real TLB
 * miss handler — the BOOT_PA_TO_VA/BOOT_VA_TO_PA macros and the
 * region table are only valid during bootstrap.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/reboot.h>

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/vmparam.h>
#include <machine/pmap.h>
#include <machine/bootinfo.h>

#include <dev/cons.h>

/* Forward declarations */
void	penumbra_init(void);
int	main(void);
static void	penumbra_physmem_init(void);
static void	*lookup_bootinfo_n(unsigned int, int);
static void	boot_regions_init(void);
static void	boot_region_add(paddr_t, paddr_t, uint32_t);

/*
 * phys_bias: physical - virtual address offset.
 * Set by locore.S during early boot.  Used to convert kernel
 * virtual addresses to physical (PA = VA + phys_bias).
 */
extern int32_t phys_bias;

/*
 * Bootstrap PA↔VA conversion using phys_bias.
 * Only valid during early boot while the bootstrap TLB handler
 * is active.  Do NOT use after pmap installs the real handler.
 */
#define BOOT_PA_TO_VA(pa)	((vaddr_t)((pa) - phys_bias))
#define BOOT_VA_TO_PA(va)	((paddr_t)((va) + phys_bias))

/*
 * Bootinfo store — locore.S copies the bootloader's bootinfo here
 * (in physical mode, before MMU enable) so it survives the address
 * space transition.  BOOTINFO_MAXSIZE (4096) is the maximum.
 */
char bootinfo_store[BOOTINFO_MAXSIZE] __aligned(4);

/* Virtual address of bootinfo_store, set by locore.S */
uint32_t penumbra_bootinfo;


/* ── Bootstrap region table ──────────────────────────────────
 *
 * The TLB miss handler uses a region table at VA 0x200 (in the
 * pinned vector page) to determine cacheability.  All VA→PA
 * translation uses a single linear mapping: PA = VA + phys_bias.
 * The table tells the handler which PA ranges are valid and
 * whether they are cached (RAM) or uncached (MMIO).
 *
 * Entry format: { pa_base, pa_end, pte_flags } (12 bytes each).
 * Sentinel: pa_base == 0 && pa_end == 0.
 *
 * locore.S pre-populates entry[0] with the kernel image.
 * boot_regions_init() adds RAM and MMIO entries from bootinfo.
 */
#define BPTE_KERNEL	0xBD		/* V|C|R|W|X|G — cached */
#define BPTE_KERNEL_NC	0x99		/* V|R|W|G — uncached */

#define REGION_TABLE	((volatile uint32_t *)0x200)
#define REGION_MAX	64		/* (0x1000 - 0x200) / 12 */

static int nregions;	/* current count (set by scanning table) */

/*
 * boot_region_add: append a region to the vector page table.
 * Called from C after MMU enable.  The vector page is accessible
 * at VA 0x0 via pinned TLB slot 0.
 */
static void
boot_region_add(paddr_t pa_base, paddr_t pa_end, uint32_t pte_flags)
{
	volatile uint32_t *entry;

	if (nregions >= REGION_MAX - 1)
		return;		/* table full */

	entry = &REGION_TABLE[nregions * 3];
	entry[0] = pa_base;
	entry[1] = pa_end;
	entry[2] = pte_flags;

	/* Write sentinel after new entry */
	nregions++;
	entry = &REGION_TABLE[nregions * 3];
	entry[0] = 0;
	entry[1] = 0;
}

/*
 * boot_regions_init: add RAM and MMIO regions from bootinfo.
 * Must be called before consinit() since the UART needs a
 * region table entry to be accessible.
 */
static void
boot_regions_init(void)
{
	struct btinfo_memory *bm;
	struct btinfo_device *bd;
	struct btinfo_console *bc;
	int i;

	/*
	 * Count existing entries (locore pre-populated entry[0]).
	 * Scan until sentinel.
	 */
	nregions = 0;
	while (REGION_TABLE[nregions * 3] != 0 ||
	       REGION_TABLE[nregions * 3 + 1] != 0)
		nregions++;

	/* Add all RAM regions (cached) */
	for (i = 0; (bm = lookup_bootinfo_n(BTINFO_MEMORY, i)) != NULL; i++)
		boot_region_add(bm->base, bm->base + bm->size, BPTE_KERNEL);

	/* Add all device MMIO regions (uncached) */
	for (i = 0; (bd = lookup_bootinfo_n(BTINFO_DEVICE, i)) != NULL; i++)
		boot_region_add(bd->addr, bd->addr + bd->size, BPTE_KERNEL_NC);

	/*
	 * Add the console device if it wasn't already covered
	 * by a BTINFO_DEVICE entry.  The console address comes
	 * from BTINFO_CONSOLE.
	 */
	bc = lookup_bootinfo(BTINFO_CONSOLE);
	if (bc != NULL)
		boot_region_add(bc->addr,
		    bc->addr + PAGE_SIZE, BPTE_KERNEL_NC);
}


/* ── Early boot UART console ──────────────────────────────────
 * 16450-compatible, word-strided registers.
 * Physical address comes from bootinfo (BTINFO_CONSOLE).
 * Virtual address computed via BOOT_PA_TO_VA() at runtime since
 * the linear mapping (PA = VA + phys_bias) may place MMIO
 * at any VA depending on where the kernel was loaded.
 */

#define UART_PA		0xFF000000	/* fallback if no bootinfo */
#define LSR_DR		0x01		/* Data Ready */
#define LSR_THRE	0x20		/* TX Holding Register Empty */

static volatile uint32_t *uart_base;

#define UART_THR	(uart_base[0])
#define UART_RBR	(uart_base[0])
#define UART_LSR	(uart_base[5])	/* word-strided: offset 0x14 */

static inline void
uart_putc(int c)
{
	while (!(UART_LSR & LSR_THRE))
		;
	UART_THR = (uint32_t)c;
}

static void penumbra_cnprobe(struct consdev *cp)
{
	cp->cn_dev = makedev(0, 0);
	cp->cn_pri = CN_REMOTE;
}

static void penumbra_cninit(struct consdev *cp)
{
	/* UART already initialized by bootloader */
}

static int penumbra_cngetc(dev_t dev)
{
	while (!(UART_LSR & LSR_DR))
		;
	return (int)UART_RBR;
}

static void penumbra_cnputc(dev_t dev, int c)
{
	uart_putc(c);
}

static void penumbra_cnpollc(dev_t dev, int on)
{
	/* nothing */
}

static struct consdev penumbra_consdev = {
	.cn_probe = penumbra_cnprobe,
	.cn_init = penumbra_cninit,
	.cn_getc = penumbra_cngetc,
	.cn_putc = penumbra_cnputc,
	.cn_pollc = penumbra_cnpollc,
	.cn_pri = CN_REMOTE,
};


/* ── Bootinfo parsing ────────────────────────────────────────── */

/*
 * lookup_bootinfo: find a bootinfo entry by type.
 * Walks the tagged list in bootinfo_store[] and returns
 * a pointer to the first entry matching `type`, or NULL.
 */
void *
lookup_bootinfo(unsigned int type)
{

	return lookup_bootinfo_n(type, 0);
}

/*
 * lookup_bootinfo_n: return the n-th entry of a given type.
 * Used to iterate over multiple entries of the same type.
 */
static void *
lookup_bootinfo_n(unsigned int type, int idx)
{
	struct bootinfo *bi = (struct bootinfo *)bootinfo_store;
	struct btinfo_common *bc;
	char *p;
	unsigned int i;
	int count = 0;

	if (bi->magic != BOOTINFO_MAGIC)
		return NULL;

	p = (char *)bi + sizeof(struct bootinfo);
	for (i = 0; i < bi->nentries; i++) {
		bc = (struct btinfo_common *)p;
		if (bc->type == type) {
			if (count == idx)
				return bc;
			count++;
		}
		p += bc->len;
	}
	return NULL;
}


/* ── Physical memory registration ────────────────────────────── */

/*
 * penumbra_physmem_init: register physical RAM with UVM.
 *
 * Walk BTINFO_MEMORY entries from bootinfo, convert each to
 * page frame numbers, exclude the kernel image, and call
 * uvm_page_physload() for each region.
 */
static void
penumbra_physmem_init(void)
{
	struct btinfo_memory *bm;
	extern char _end[];
	int i;

	/*
	 * Walk all BTINFO_MEMORY entries.
	 * The bootloader creates one entry per RAM region.
	 */
	for (i = 0; (bm = lookup_bootinfo_n(BTINFO_MEMORY, i)) != NULL; i++) {
		paddr_t seg_start = bm->base;
		paddr_t seg_end = bm->base + bm->size;
		paddr_t avail_start;

		printf("  RAM: 0x%x - 0x%x (%u KB)\n",
		    (unsigned)seg_start, (unsigned)seg_end,
		    (unsigned)(bm->size / 1024));

		/*
		 * The kernel is loaded somewhere in physical RAM.
		 * Exclude everything below the kernel BSS end so
		 * we don't hand kernel pages to UVM.  phys_bias
		 * converts the kernel virtual _end to physical.
		 */
		avail_start = round_page(BOOT_VA_TO_PA((vaddr_t)&_end));
		if (avail_start > seg_start && avail_start < seg_end)
			seg_start = avail_start;

		/* Skip regions entirely below kernel end */
		if (seg_start >= seg_end)
			continue;

		printf("  physload: PFN 0x%x - 0x%x (PA 0x%x - 0x%x)\n",
		    (unsigned)atop(seg_start), (unsigned)atop(seg_end),
		    (unsigned)seg_start, (unsigned)seg_end);
		uvm_page_physload(
		    atop(seg_start), atop(seg_end),
		    atop(seg_start), atop(seg_end),
		    VM_FREELIST_DEFAULT);
	}
}


/* ── penumbra_init: main early boot entry point ──────────────── */

/*
 * Early machine initialization.
 * Called from locore.S after BSS is zeroed and bootinfo copied.
 */
void
penumbra_init(void)
{

	/*
	 * Add RAM and MMIO regions to the bootstrap TLB handler's
	 * region table.  Must happen before any MMIO access (console).
	 */
	boot_regions_init();

	/*
	 * Set up UART base address.  Use bootinfo console address
	 * if available, otherwise fall back to hardcoded default.
	 * Convert PA to VA via the linear mapping.
	 */
	{
		struct btinfo_console *bc = lookup_bootinfo(BTINFO_CONSOLE);
		paddr_t uart_pa = (bc != NULL) ? bc->addr : UART_PA;
		uart_base = (volatile uint32_t *)BOOT_PA_TO_VA(uart_pa);
	}

	/* Initialize the console so we can printf */
	consinit();

	printf("NetBSD/penumbra booting\n");
	printf("phys_bias = 0x%x\n", (unsigned)phys_bias);
	printf("Regions (%d):\n", nregions);
	{
		int r;
		for (r = 0; r < nregions; r++)
			printf("  [%d] PA 0x%x-0x%x flags 0x%x\n", r,
			    (unsigned)REGION_TABLE[r * 3],
			    (unsigned)REGION_TABLE[r * 3 + 1],
			    (unsigned)REGION_TABLE[r * 3 + 2]);
	}

	/* Set page size and initialize physseg bookkeeping */
	uvm_md_init();

	/* Register physical memory with UVM */
	penumbra_physmem_init();

	/* Initialize pmap (page table / TLB management) */
	pmap_bootstrap();

	/* Hand off to MI kernel main */
	main();
	/* NOTREACHED */
}

/*
 * Console initialization — called very early.
 * Sets up UART for printf.
 */
void
consinit(void)
{

	cn_tab = &penumbra_consdev;
	penumbra_cnprobe(cn_tab);
	penumbra_cninit(cn_tab);
}

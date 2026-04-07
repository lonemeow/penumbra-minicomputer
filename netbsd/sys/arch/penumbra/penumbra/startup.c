/*	$NetBSD$	*/

/*
 * Penumbra early boot initialization.
 *
 * This file contains everything needed to get from locore.S handoff
 * to main(): bootinfo parsing, early console, physical memory
 * registration, and the penumbra_init() entry point itself.
 *
 * By the time penumbra_init() runs, locore.S has already:
 *   - Zeroed BSS, copied bootinfo
 *   - Built the kernel page table (L1 + L2 in BSS)
 *   - Installed the real TLB miss handler
 *   - Pinned the L1 in TLB slot 1
 *   - Enabled the MMU
 *
 * Only the kernel image (text through BSS+boot tables) is mapped.
 * MMIO devices need explicit mapping via pmap.
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

/*
 * Bootinfo store — locore.S copies the bootloader's bootinfo here
 * (in physical mode, before MMU enable) so it survives the address
 * space transition.  BOOTINFO_MAXSIZE (4096) is the maximum.
 */
char bootinfo_store[BOOTINFO_MAXSIZE] __aligned(4);

/* Virtual address of bootinfo_store, set by locore.S */
uint32_t penumbra_bootinfo;


/* ── Early boot UART console ──────────────────────────────────
 * 16450-compatible, word-strided registers.
 * Physical address comes from bootinfo (BTINFO_CONSOLE).
 * Initially mapped via pinned TLB slot 3 (scratch window).
 * pmap_bootstrap() remaps at a proper kernel VA and updates
 * uart_base.
 */

#define UART_PA		0xFF000000	/* fallback if no bootinfo */
#define LSR_DR		0x01		/* Data Ready */
#define LSR_THRE	0x20		/* TX Holding Register Empty */

volatile uint32_t *uart_base;

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
void *
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
	struct btinfo_kernbase *bk;
	paddr_t kern_pa_end;
	int i;

	/*
	 * Determine the physical end of the loaded kernel.
	 * The bootloader loads text+data+bss AND the symbol table
	 * contiguously starting at phys_base.  Compute the physical
	 * end from phys_base + total virtual size (kern_end includes
	 * symtab).  Fall back to _end if no bootinfo.
	 */
	bk = lookup_bootinfo(BTINFO_KERNBASE);
	if (bk != NULL) {
		vsize_t kern_vsize = bk->kern_end - bk->kern_start;
		kern_pa_end = round_page(bk->phys_base + kern_vsize);
	} else {
		extern char _end[];
		kern_pa_end = round_page(
		    (paddr_t)((vaddr_t)&_end + phys_bias));
	}

	printf("  kernel PA end: 0x%x\n", (unsigned)kern_pa_end);

	/*
	 * Walk all BTINFO_MEMORY entries.
	 * The bootloader creates one entry per RAM region.
	 */
	for (i = 0; (bm = lookup_bootinfo_n(BTINFO_MEMORY, i)) != NULL; i++) {
		paddr_t seg_start = bm->base;
		paddr_t seg_end = bm->base + bm->size;

		printf("  RAM: 0x%x - 0x%x (%u KB)\n",
		    (unsigned)seg_start, (unsigned)seg_end,
		    (unsigned)(bm->size / 1024));

		/*
		 * Exclude the kernel's physical footprint (text +
		 * data + bss + symbol table) from this region.
		 */
		if (kern_pa_end > seg_start && kern_pa_end < seg_end)
			seg_start = kern_pa_end;

		/* Skip regions entirely covered by kernel */
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
	 * Set up UART for early printf.
	 *
	 * The real TLB miss handler is already active (locore.S
	 * installed it before MMU enable), but only the kernel image
	 * is mapped.  The UART is at a high PA with no PTE yet.
	 *
	 * Temporarily pin the UART via TLB slot 3 (scratch window)
	 * so printf works during early boot.  pmap_bootstrap() will
	 * create a proper page table mapping and update uart_base.
	 */
	{
		struct btinfo_console *bc = lookup_bootinfo(BTINFO_CONSOLE);
		paddr_t uart_pa = (bc != NULL) ? bc->addr : UART_PA;
		pmap_scratch_map(trunc_page(uart_pa), PTE_KERNEL_NC);
		uart_base = (volatile uint32_t *)
		    (SCRATCH_VA + (uart_pa & PAGE_MASK));
	}

	/* Initialize the console so we can printf */
	consinit();

	printf("NetBSD/penumbra booting\n");
	printf("phys_bias = 0x%x\n", (unsigned)phys_bias);

	/* Set page size and initialize physseg bookkeeping */
	uvm_md_init();

	/* Register physical memory with UVM */
	penumbra_physmem_init();

	/*
	 * Initialize pmap — adopts locore's page table, sets up
	 * virtual_avail for UVM.  No device knowledge.
	 */
	pmap_bootstrap();

	/*
	 * Map the console UART permanently via pmap.
	 * Until now it was accessible through the scratch window
	 * (pinned slot 3).  pmap_map_device allocates a kernel VA
	 * and installs a proper PTE, so the TLB miss handler can
	 * resolve UART accesses.  The scratch window is now free
	 * for other uses (pmap_steal_memory, pmap_zero_page, etc.).
	 */
	{
		struct btinfo_console *bc = lookup_bootinfo(BTINFO_CONSOLE);
		paddr_t uart_pa = (bc != NULL) ? bc->addr : UART_PA;
		vaddr_t uart_kva = pmap_map_device(trunc_page(uart_pa),
		    PAGE_SIZE);
		uart_base = (volatile uint32_t *)
		    (uart_kva + (uart_pa & PAGE_MASK));
		printf("console: UART PA 0x%x -> VA 0x%x\n",
		    (unsigned)uart_pa, (unsigned)(vaddr_t)uart_base);
	}

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

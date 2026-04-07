/*	$NetBSD$	*/

/*
 * Penumbra physical map (pmap) — software TLB management.
 *
 * The Penumbra MMU has a 64-entry, 2-way set-associative,
 * fully software-managed TLB.  The OS controls all TLB loads,
 * evictions, and invalidations via WRSYS/RDSYS instructions.
 *
 * Page table layout — always 2-level:
 *   L1: 1024 entries × 4 bytes = 4 KB (one page)
 *   L2:  1024 entries × 4 bytes = 4 KB (covers 4 MB each)
 *
 * No direct-map.  All mappings are explicit page table entries.
 * Physical pages with no kernel VA are accessed via a scratch
 * window (pinned TLB slot 3 at SCRATCH_VA).
 *
 * Pinned TLB allocation:
 *   Slot 0: Vector page (VECTOR_VA 0xFFFFB000) — handler code, scratch
 *   Slot 1: Current L1 table (VA 0xFFFFE000) — updated on ctx switch
 *   Slot 2: L2 window (VA 0xFFFFD000) — handler's transient L2
 *   Slot 3: Scratch window (VA 0xFFFFC000) — C code page access
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/mutex.h>

#include <uvm/uvm.h>

#include <machine/cpu.h>
#include <machine/pmap.h>
#include <machine/vmparam.h>
#include <machine/psl.h>
#include <machine/sysreg.h>
#include <machine/bootinfo.h>

/* Linker-defined symbols */
extern char _end[];

/* Kernel pmap */
struct pmap kernel_pmap_store;
struct pmap *const kernel_pmap_ptr = &kernel_pmap_store;

/* Virtual address range available for kernel VM allocation */
static vaddr_t virtual_avail;
static vaddr_t virtual_end;

/*
 * Two-phase L2 allocation flag.
 *
 * Before pmap_init(): UVM is not yet operational, so L2 tables
 * must be allocated by stealing pages directly from physseg.
 * After pmap_init(): UVM is ready, use uvm_pagealloc().
 *
 * pmap_init() sets this to true.  pmap_alloc_l2() checks it
 * to decide which allocator to use.
 */
static bool pmap_initialized;


/* ── Scratch window ───────────────────────────────────────────
 *
 * Temporarily map a physical page at SCRATCH_VA via pinned TLB
 * slot 3.  Used to access pages that have no kernel VA mapping:
 * freshly stolen pages, new L2 tables, pmap_zero_page, etc.
 *
 * pmap_scratch_map() is in locore.S (needs WRSYS).
 *
 * IMPORTANT: The scratch window is a single shared resource.
 * All code that uses pmap_scratch_map() / pmap_l2_map() must
 * hold splhigh() to prevent interrupt handlers from re-entering
 * pmap and clobbering the mapping.  The TLB miss handler is safe
 * (uses a separate L2 window in pinned slot 2), but any interrupt
 * that triggers pmap operations would corrupt the scratch mapping.
 * Early boot code (before interrupts are possible) is exempt.
 */

void *
pmap_scratch_va(void)
{
	return (void *)SCRATCH_VA;
}


/* ── Dynamic L2 allocation ───────────────────────────────────
 *
 * When pmap_kenter_pa or pmap_steal_memory encounters an L1 entry
 * with no L2 table, this helper allocates a new L2 page, zeros it,
 * and installs it in the L1.
 *
 * Two-phase: before pmap_init(), steal from physseg directly;
 * after pmap_init(), use uvm_pagealloc().
 */

/*
 * pmap_alloc_l2: allocate a zeroed L2 table and install it in L1.
 *
 * Two-phase: before pmap_init(), steal from physseg;
 * after pmap_init(), use uvm_pagealloc with UVM_PGA_USERESERVE
 * (L2 allocation is critical — dipping into reserve is justified).
 */
static void
pmap_alloc_l2(pt_entry_t *l1, unsigned int l1_idx)
{
	paddr_t pa;
	
	if (!pmap_initialized)
	{
		pa = pmap_steal_page();
	}
	else
	{
		struct vm_page *page = uvm_pagealloc(NULL, 0, NULL, UVM_PGA_USERESERVE | UVM_PGA_ZERO);
		if (!page)
			panic("pmap: Unable to allocate L2 page table page");
		pa = VM_PAGE_TO_PHYS(page);
	}

	l1[l1_idx] = PT_L1E_MAKE(pa);
}


/* ── L2 table access helpers ──────────────────────────────────
 *
 * L2 tables are physical pages.  To read/write them from C code,
 * we map them into the scratch window.  The L1 table is always
 * accessible at PT_L1_VA (pinned slot 1).
 *
 * For pmap_kenter_pa / pmap_kremove (hot path), the L2 table
 * may already be the kernel image's BSS-resident L2, which IS
 * mapped in the kernel VA range.  For those, we can access them
 * directly.  For dynamically allocated L2 tables, we must use
 * the scratch window.
 *
 * To keep things simple initially, we always use the scratch
 * window for L2 access.
 */

/*
 * Map an L2 table into the scratch window and return a pointer.
 */
static pt_entry_t *
pmap_l2_map(paddr_t l2_pa)
{
	pmap_scratch_map(l2_pa, PTE_KERNEL);
	return (pt_entry_t *)SCRATCH_VA;
}


/* ── pmap_bootstrap ───────────────────────────────────────────
 *
 * Called from penumbra_init() after physical memory is registered
 * with UVM.  Adopts the L1 and L2 tables that locore.S built
 * in BSS (physical mode).  The real TLB miss handler and pinned
 * L1 are already active — locore.S set them up before enabling
 * the MMU.
 *
 * Maps the console UART at a kernel VA (MMIO can't be in the
 * kernel image VA range because MMIO PAs are high and would
 * need separate L2 tables).
 */
void
pmap_bootstrap(void)
{

	/* Initialize kernel pmap */
	mutex_init(&kernel_pmap_store.pm_lock, MUTEX_DEFAULT, IPL_VM);
	kernel_pmap_store.pm_asid = 0;	/* kernel uses ASID 0 + G=1 */
	kernel_pmap_store.pm_count = 1;

	/*
	 * Adopt the L1 table that locore.S built in BSS.
	 * It's already pinned in TLB slot 1, and populated with
	 * L2 entries covering the kernel image.
	 */
	kernel_pmap_store.pm_l1 = (pt_entry_t *)_boot_l1;
	kernel_pmap_store.pm_l1_pa = (paddr_t)((vaddr_t)_boot_l1 + phys_bias);

	printf("pmap: L1 at VA 0x%x PA 0x%x\n",
	    (unsigned)(vaddr_t)_boot_l1,
	    (unsigned)kernel_pmap_store.pm_l1_pa);

	/*
	 * virtual_avail: first kernel VA available for dynamic mapping.
	 * Must be past the entire loaded kernel image including the
	 * symbol table (which the bootloader places after BSS).
	 * BTINFO_KERNBASE.kern_end gives the true virtual end.
	 * Fall back to _end (BSS end) if bootinfo is missing.
	 */
	{
		struct btinfo_kernbase *bk = lookup_bootinfo(BTINFO_KERNBASE);
		if (bk != NULL)
			virtual_avail = round_page(bk->kern_end);
		else
			virtual_avail = round_page((vaddr_t)&_end);
	}
	virtual_end = VM_MAX_KERNEL_ADDRESS;

	printf("pmap: virtual_avail = 0x%x, virtual_end = 0x%x\n",
	    (unsigned)virtual_avail, (unsigned)virtual_end);
}

/*
 * pmap_map_device: map a physical device page into kernel VA space.
 *
 * Allocates a kernel VA from virtual_avail and maps it uncached
 * via pmap_kenter_pa.  Returns the kernel VA (page-aligned).
 * Used for early MMIO device mapping (UART, etc.) before UVM's
 * bus_space is available.
 *
 * The L2 table for this VA must already exist (covered by the
 * BSS-allocated L2 tables) or pmap_kenter_pa will panic.
 */
vaddr_t
pmap_map_device(paddr_t pa, vsize_t size)
{
	vaddr_t va = virtual_avail;

	size = round_page(size);
	pa = trunc_page(pa);
	virtual_avail += size;

	for (vsize_t off = 0; off < size; off += PAGE_SIZE)
		pmap_kenter_pa(va + off, pa + off,
		    VM_PROT_READ | VM_PROT_WRITE, PMAP_NOCACHE);

	return va;
}

/*
 * pmap_virtual_space: report available kernel virtual address range.
 * Called by UVM during initialization.
 */
void
pmap_virtual_space(vaddr_t *vstartp, vaddr_t *vendp)
{
	*vstartp = virtual_avail;
	*vendp = virtual_end;
}

/*
 * pmap_steal_memory: allocate physical pages during early boot.
 *
 * Steals physical pages from UVM physseg, maps them at
 * virtual_avail in the kernel page table, and returns the VA.
 * Uses the scratch window to access the L2 table (and to
 * zero the stolen page if it's not yet mapped).
 */
vaddr_t
pmap_steal_memory(vsize_t size, vaddr_t *vstartp, vaddr_t *vendp)
{
	uvm_physseg_t bank;
	int npgs;
	paddr_t pa;

	size = round_page(size);
	npgs = atop(size);

	for (bank = uvm_physseg_get_first();
	     uvm_physseg_valid_p(bank);
	     bank = uvm_physseg_get_next(bank)) {
		if (uvm_physseg_get_avail_end(bank) -
		    uvm_physseg_get_avail_start(bank) < npgs)
			continue;

		/* Steal from the start of the available region */
		pa = ptoa(uvm_physseg_get_avail_start(bank));
		uvm_physseg_unplug(atop(pa), npgs);

		/*
		 * Map each stolen page at virtual_avail.
		 * Install PTEs in the kernel page table.
		 */
		vaddr_t va = *vstartp;
		vaddr_t va_cur = va;
		paddr_t pa_cur = pa;
		int i;

		for (i = 0; i < npgs; i++, va_cur += PAGE_SIZE,
		    pa_cur += PAGE_SIZE) {
			pt_entry_t *l1 = kernel_pmap_store.pm_l1;
			unsigned int l1_idx = PT_L1_INDEX(va_cur);

			if (!(l1[l1_idx] & PTE_V))
				pmap_alloc_l2(l1, l1_idx);

			/* Map L2 via scratch, install PTE */
			paddr_t l2_pa = l1[l1_idx] & PTE_PPN_MASK;
			pt_entry_t *l2 = pmap_l2_map(l2_pa);
			l2[PT_L2_INDEX(va_cur)] =
			    (pa_cur & PTE_PPN_MASK) | PTE_KERNEL;
		}

		*vstartp = va + size;

		/* Zero the stolen pages via scratch window */
		pa_cur = pa;
		for (i = 0; i < npgs; i++, pa_cur += PAGE_SIZE) {
			pmap_scratch_map(pa_cur, PTE_KERNEL);
			memset((void *)SCRATCH_VA, 0, PAGE_SIZE);
		}

		return va;
	}
	panic("pmap_steal_memory: no memory to steal %u bytes", (unsigned)size);
}

paddr_t
pmap_steal_page(void)
{
	uvm_physseg_t bank;

	for (bank = uvm_physseg_get_first();
	     uvm_physseg_valid_p(bank);
	     bank = uvm_physseg_get_next(bank)) {
		if (uvm_physseg_get_avail_end(bank) -
		    uvm_physseg_get_avail_start(bank) < 1)
			continue;

		/* Steal from the start of the available region */
		paddr_t pa = ptoa(uvm_physseg_get_avail_start(bank));
		uvm_physseg_unplug(atop(pa), 1);
		/* Zero the stolen page */
		pmap_scratch_map(pa, PTE_KERNEL);
		memset((void *)SCRATCH_VA, 0, PAGE_SIZE);
		return pa;
	}

	panic("pmap: Unable to steal page");
}

/*
 * pmap_init: called after UVM is operational.
 * Allocate pools for pmap structures, PTEs, etc.
 */
void
pmap_init(void)
{

	/*
	 * Switch L2 allocation from physseg stealing to uvm_pagealloc.
	 * Must happen before any other pmap_init work, since pool
	 * setup may trigger pmap_kenter_pa → pmap_alloc_l2.
	 */
	pmap_initialized = true;
	printf("pmap_init: done\n");

	/* TODO: initialize PTE pools, ASID allocator */
}

/*
 * pmap_create: create a new user pmap.
 */
pmap_t
pmap_create(void)
{
	/* TODO: allocate pmap + L1 table, copy kernel L1 entries */
	return NULL;
}

/*
 * pmap_destroy: free a user pmap.
 */
void
pmap_destroy(pmap_t pm)
{
	/* TODO */
}

void
pmap_reference(pmap_t pm)
{
	pm->pm_count++;
}

/*
 * pmap_enter: create a mapping in a user or kernel pmap.
 */
int
pmap_enter(pmap_t pm, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	panic("pmap_enter: not yet implemented (va=0x%x pa=0x%x)",
	    (unsigned)va, (unsigned)pa);
}

/*
 * pmap_remove: remove mappings in range.
 */
void
pmap_remove(pmap_t pm, vaddr_t sva, vaddr_t eva)
{
	panic("pmap_remove: not yet implemented (0x%x-0x%x)",
	    (unsigned)sva, (unsigned)eva);
}

/*
 * pmap_protect: change protection on a range.
 */
void
pmap_protect(pmap_t pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{
	panic("pmap_protect: not yet implemented (0x%x-0x%x)",
	    (unsigned)sva, (unsigned)eva);
}

/*
 * pmap_unwire: clear wired attribute.
 */
void
pmap_unwire(pmap_t pm, vaddr_t va)
{
	panic("pmap_unwire: not yet implemented (va=0x%x)",
	    (unsigned)va);
}

/*
 * pmap_extract: look up the physical address for a VA.
 */
bool
pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap)
{
	pt_entry_t *l1 = pm->pm_l1;
	unsigned int l1_idx = PT_L1_INDEX(va);

	if (!(l1[l1_idx] & PTE_V))
		return false;

	int s = splhigh();
	paddr_t l2_pa = l1[l1_idx] & PTE_PPN_MASK;
	pt_entry_t *l2 = pmap_l2_map(l2_pa);
	pt_entry_t pte = l2[PT_L2_INDEX(va)];
	splx(s);

	if (!(pte & PTE_V))
		return false;

	if (pap != NULL)
		*pap = (pte & PTE_PPN_MASK) | (va & PAGE_MASK);

	return true;
}

/*
 * pmap_kenter_pa: create a wired kernel mapping.
 *
 * Maps a single page at kernel VA to physical address PA with
 * the given protection.  The mapping is wired (never paged out)
 * and global (G=1, shared across all address spaces).
 *
 * Called by UVM to map dynamically allocated kernel pages.
 * If no L2 table exists for this VA range, one is allocated
 * via the scratch window.
 */
void
pmap_kenter_pa(vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	pt_entry_t *l1 = kernel_pmap_store.pm_l1;
	unsigned int l1_idx = PT_L1_INDEX(va);

	int s = splhigh();

	if (!(l1[l1_idx] & PTE_V))
		pmap_alloc_l2(l1, l1_idx);

	paddr_t l2_pa = l1[l1_idx] & PTE_PPN_MASK;
	pt_entry_t *l2 = pmap_l2_map(l2_pa);

	l2[PT_L2_INDEX(va)] = PTE_MAKE(pa, prot, flags, PTE_G);
	tlb_invalidate_addr(va, 0);

	splx(s);
}

/*
 * pmap_kremove: remove wired kernel mappings.
 *
 * Removes len bytes of wired kernel mappings starting at va.
 * Must invalidate TLB entries for the removed range.
 */
void
pmap_kremove(vaddr_t va, vsize_t len)
{
	pt_entry_t *l1 = kernel_pmap_store.pm_l1;
	vaddr_t eva = va + len;

	int s = splhigh();
	for (; va < eva; va += PAGE_SIZE) {
		unsigned int l1_idx = PT_L1_INDEX(va);
		if (!(l1[l1_idx] & PTE_V))
			continue;

		paddr_t l2_pa = l1[l1_idx] & PTE_PPN_MASK;
		pt_entry_t *l2 = pmap_l2_map(l2_pa);
		unsigned int l2_idx = PT_L2_INDEX(va);

		if (l2[l2_idx] & PTE_V) {
			l2[l2_idx] = 0;
			tlb_invalidate_addr(va, 0);
		}
	}
	splx(s);
}

/*
 * pmap_copy: advisory — copy mappings from one pmap to another.
 */
void
pmap_copy(pmap_t dst, pmap_t src, vaddr_t dstaddr, vsize_t len, vaddr_t srcaddr)
{
	/* nothing — optional optimization */
}

/*
 * pmap_activate / pmap_deactivate: switch active pmap.
 */
void
pmap_activate(struct lwp *l)
{
	/* TODO: load ASID into MMUCR, re-pin L1 in slot 1 */
}

void
pmap_deactivate(struct lwp *l)
{
	/* nothing needed */
}

/*
 * pmap_zero_page / pmap_copy_page: page operations.
 * Operate on physical pages via the scratch window.
 */
void
pmap_zero_page(paddr_t pa)
{
	int s = splhigh();
	pmap_scratch_map(pa, PTE_KERNEL);
	memset((void *)SCRATCH_VA, 0, PAGE_SIZE);
	splx(s);
}

void
pmap_copy_page(paddr_t src, paddr_t dst)
{
	static char buf[PAGE_SIZE] __aligned(4);

	int s = splhigh();
	/* Read source via scratch window into a temp buffer */
	pmap_scratch_map(src, PTE_KERNEL);
	memcpy(buf, (const void *)SCRATCH_VA, PAGE_SIZE);

	/* Write temp buffer to destination via scratch window */
	pmap_scratch_map(dst, PTE_KERNEL);
	memcpy((void *)SCRATCH_VA, buf, PAGE_SIZE);
	splx(s);
}

/*
 * pmap_page_protect: downgrade all mappings of a page.
 */
void
pmap_page_protect(struct vm_page *pg, vm_prot_t prot)
{
	/* TODO */
}

/*
 * pmap_clear_modify / pmap_clear_reference / pmap_is_modified / pmap_is_referenced:
 * Dirty/reference tracking via software PTE bits.
 */
bool
pmap_clear_modify(struct vm_page *pg)
{
	/* TODO */
	return false;
}

bool
pmap_clear_reference(struct vm_page *pg)
{
	/* TODO */
	return false;
}

bool
pmap_is_modified(struct vm_page *pg)
{
	/* TODO */
	return false;
}

bool
pmap_is_referenced(struct vm_page *pg)
{
	/* TODO */
	return false;
}

/*
 * TLB invalidation is implemented in locore.S (WRSYS/RDSYS).
 * Declarations: tlb_invalidate_all/asid/addr in pmap.h.
 */

/*
 * pmap_remove_all — remove all mappings from a pmap.
 * Called on process exit.
 */
bool
pmap_remove_all(struct pmap *pmap)
{
	/* TODO(stub): walk page table, free entries, invalidate TLB */
	return false;
}

/*
 * pmap_phys_address — convert page frame to physical address.
 */
paddr_t
pmap_phys_address(paddr_t frame)
{
	return ptoa(frame);
}

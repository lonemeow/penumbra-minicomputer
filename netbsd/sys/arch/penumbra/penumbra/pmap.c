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
#include <sys/kmem.h>

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

/*
 * ASID allocator — generational.
 *
 * Each pmap's ASID is tagged with a generation number.  On context
 * switch, pmap_activate() compares the pmap's generation against
 * the global generation; if stale, a fresh ASID is allocated.
 *
 * 8-bit ASID space: 0 is reserved for the kernel (all kernel TLB
 * entries use G=1, which bypasses ASID matching), so user ASIDs
 * range from 1–255.  When the pool is exhausted, the generation
 * bumps, the entire TLB is flushed (all stale-ASID entries gone),
 * and allocation restarts from 1.
 *
 * This avoids a full TLB flush on every context switch — the flush
 * only happens once every 255 distinct user pmap activations.
 */
static uint32_t pmap_asid_generation = 1;
static uint8_t  pmap_asid_next = 1;

/*
 * pmap_asid_alloc: assign a fresh ASID to a pmap.
 *
 * Called from pmap_activate() when pm->pm_asid_gen doesn't match
 * pmap_asid_generation (i.e., the pmap's ASID is stale or was
 * never assigned).
 *
 * Must handle exhaustion of the 1–255 range: bump the global
 * generation, flush the TLB, and restart allocation from 1.
 *
 * After return: pm->pm_asid is a valid ASID (1–255) and
 * pm->pm_asid_gen == pmap_asid_generation.
 */
static void
pmap_asid_alloc(struct pmap *pm)
{

	/* Caller must hold splhigh() — called from pmap_activate(). */
	if (pmap_asid_next > 255) {
		pmap_asid_generation++;
		pmap_asid_next = 1;
		tlb_invalidate_all();
	}

	pm->pm_asid = pmap_asid_next++;
	pm->pm_asid_gen = pmap_asid_generation;
}

/*
 * Write MMUCR: set MMU-enable + ASID field.
 * Called from pmap_activate() on every context switch.
 */
static inline void
pmap_set_mmucr(uint8_t asid)
{
	uint32_t val = MMUCR_M | ((uint32_t)asid << MMUCR_ASID_SHIFT);

	__asm__ volatile(
	    "WRSYS %0, %1, %2" : : "r"(val),
	    "n"(SYSDEV_MMU), "n"(MMU_MMUCR));
}

/*
 * Invalidate a TLB entry for a VA in the context of a specific pmap.
 *
 * Kernel pmap: always ASID 0 (entries use G=1).
 * User pmap with current generation: invalidate with pm_asid.
 * User pmap with stale generation: skip — TLB was already flushed
 * when the generation bumped, so no matching entries can exist.
 */
static inline void
pmap_tlb_invalidate(struct pmap *pm, vaddr_t va)
{

	if (pm == pmap_kernel())
		tlb_invalidate_addr(va, 0);
	else if (pm->pm_asid_gen == pmap_asid_generation)
		tlb_invalidate_addr(va, pm->pm_asid);
}


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
 *
 * INVARIANT: Kernel L2 tables are never freed.  User pmaps get
 * a snapshot of the kernel L1 at pmap_create() time; entries
 * added later are lazily propagated by trap.c on TLB miss.
 * If kernel L2 tables were freed, user pmaps would hold stale
 * L1 entries pointing to recycled pages — silent corruption.
 * To add kernel L2 reclamation, switch to eager propagation
 * (maintain a list of active user pmaps and update all L1s).
 */
static bool
pmap_alloc_l2(pt_entry_t *l1, unsigned int l1_idx)
{
	paddr_t pa;

	if (!pmap_initialized) {
		pa = pmap_steal_page();
	} else {
		struct vm_page *pg = uvm_pagealloc(NULL, 0, NULL,
		    UVM_PGA_USERESERVE | UVM_PGA_ZERO);
		if (pg == NULL)
			return false;
		pa = VM_PAGE_TO_PHYS(pg);
	}

	l1[l1_idx] = PT_L1E_MAKE(pa);
	return true;
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

/*
 * pmap_pte_lookup: walk L1→L2 and return a pointer to the PTE
 * for the given VA.  Returns NULL if the L1 entry is invalid
 * (no L2 table allocated for this VA range).
 *
 * The returned pointer is into the scratch window and is only
 * valid until the next pmap_l2_map/pmap_pte_lookup/pmap_scratch_map.
 * Caller must hold splhigh().
 */
static pt_entry_t *
pmap_pte_lookup(pt_entry_t *l1, vaddr_t va)
{
	unsigned int l1_idx = PT_L1_INDEX(va);

	if (!(l1[l1_idx] & PTE_V))
		return NULL;

	pt_entry_t *l2 = pmap_l2_map(l1[l1_idx] & PTE_PPN_MASK);
	return &l2[PT_L2_INDEX(va)];
}


/* ── PV list management ──────────────────────────────────────
 *
 * Each UVM-managed physical page has a linked list of PV entries
 * tracking every virtual mapping.  This allows pmap_page_protect()
 * to find and modify/remove all PTEs that map a given page —
 * critical for COW (fork), pageout, and page replacement.
 *
 * PV entries are pool-allocated.  The list head lives in
 * vm_page_md (pg->mdpage.pvh_list).
 */
static struct pool pv_pool;

/*
 * pv_enter: insert a pre-allocated PV entry into a page's PV list.
 * Caller must hold splhigh().
 */
static void
pv_enter(struct vm_page *pg, struct pv_entry *pv,
    struct pmap *pm, vaddr_t va)
{
	struct vm_page_md *md = VM_PAGE_TO_MD(pg);

	pv->pv_pmap = pm;
	pv->pv_va = va;
	SLIST_INSERT_HEAD(&md->pvh_list, pv, pv_link);
}

/*
 * pv_remove: find and remove the PV entry for (pm, va) from a page's
 * PV list.  Frees the entry back to pv_pool.
 * Caller must hold splhigh().
 */
static void
pv_remove(struct vm_page *pg, struct pmap *pm, vaddr_t va)
{
	struct vm_page_md *md = VM_PAGE_TO_MD(pg);
	struct pv_entry *pv;

	SLIST_FOREACH(pv, &md->pvh_list, pv_link) {
		if (pv->pv_pmap == pm && pv->pv_va == va) {
			SLIST_REMOVE(&md->pvh_list, pv, pv_entry, pv_link);
			pool_put(&pv_pool, pv);
			return;
		}
	}
	/* Not found — may have been removed by pmap_page_protect already */
}


/*
 * Install kernel PTEs for the "image tail" — the region between
 * round_page(_end) and bk->kern_end where the bootloader appends
 * the symbol table (and any other post-BSS payload).
 *
 * locore.S maps only [KERN_TEXT_VA, round_page(_end)) because it
 * runs before bootinfo is parsed and has no way to know how far
 * the loader extended the image.  We complete the mapping here.
 *
 * Deferred out of pmap_bootstrap because pmap_kenter_pa reloads the
 * scratch-window pinned TLB slot to access target L2 pages — and
 * during pmap_bootstrap the same scratch slot is the only mapping
 * for the early UART.  Once startup.c has remapped the UART through
 * pmap_map_device, the scratch slot is free for reuse and we can
 * call this safely.
 *
 * The L2 tables for these VAs already exist (BOOT_NL2 in locore.S
 * covers well past kern_end for any plausible kernel size), so
 * pmap_kenter_pa just fills in PTEs — no L2 allocation needed.
 */
void
pmap_map_kernel_tail(void)
{
	struct btinfo_kernbase *bk;
	vaddr_t end_va = round_page((vaddr_t)&_end);
	vaddr_t kern_end_va;
	paddr_t phys_off;

	bk = lookup_bootinfo(BTINFO_KERNBASE);
	if (bk == NULL)
		return;

	kern_end_va = round_page(bk->kern_end);
	phys_off = bk->kern_start - bk->phys_base;

	/*
	 * RW because ksyms_addsyms_elf rewrites the ELF header in place
	 * (ksyms_hdr_init() in kern/kern_ksyms.c) before parsing it.
	 */
	for (vaddr_t va = end_va; va < kern_end_va; va += PAGE_SIZE)
		pmap_kenter_pa(va, va - phys_off,
		    VM_PROT_READ | VM_PROT_WRITE, 0);
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
	kernel_pmap_store.pm_asid = 0;
	kernel_pmap_store.pm_asid_gen = 0;	/* kernel: always ASID 0 + G=1 */
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
	 * Reserve VA past the loaded kernel tail (symbol table region).
	 * The actual PTE installation is deferred to pmap_map_kernel_tail()
	 * called later from cpu_startup() — see that function's comment.
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

			if (!(l1[l1_idx] & PTE_V)) {
				if (!pmap_alloc_l2(l1, l1_idx))
					panic("pmap_steal_memory: L2 alloc");
			}

			pt_entry_t *ptep = pmap_pte_lookup(l1, va_cur);
			*ptep = (pa_cur & PTE_PPN_MASK) | PTE_KERNEL;
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

	/* Initialize PV entry pool */
	pool_init(&pv_pool, sizeof(struct pv_entry), 0, 0, 0,
	    "pvpl", NULL, IPL_VM);

	printf("pmap_init: done\n");
}

/*
 * pmap_create: create a new user pmap.
 *
 * Allocates a pmap struct and an L1 page table.
 * Copies the kernel half of the L1 (entries covering
 * VM_MIN_KERNEL_ADDRESS and above) so supervisor-mode
 * code can access kernel memory in any address space.
 * The user half is zeroed (no user mappings yet).
 */
pmap_t
pmap_create(void)
{
	struct pmap *pm;

	pm = kmem_alloc(sizeof(*pm), KM_SLEEP);
	memset(pm, 0, sizeof(*pm));
	mutex_init(&pm->pm_lock, MUTEX_DEFAULT, IPL_VM);
	pm->pm_count = 1;
	pm->pm_asid = 0;
	pm->pm_asid_gen = 0;	/* force ASID allocation on first activate */

	/*
	 * Allocate L1 table (one physical page).
	 * Zero the user half, copy the kernel half from kernel_pmap.
	 */
	struct vm_page *pg = uvm_pagealloc(NULL, 0, NULL,
	    UVM_PGA_USERESERVE | UVM_PGA_ZERO);
	if (pg == NULL)
		panic("pmap_create: cannot allocate L1 table");
	pm->pm_l1_pa = VM_PAGE_TO_PHYS(pg);

	/*
	 * We need a kernel VA to access the new L1.  Use uvm_km_alloc
	 * to get one page of KVA, then wire it to the L1's PA.
	 */
	vaddr_t va = uvm_km_alloc(kernel_map, PAGE_SIZE, 0,
	    UVM_KMF_VAONLY | UVM_KMF_WAITVA);
	if (va == 0)
		panic("pmap_create: cannot allocate KVA for L1");
	pmap_kenter_pa(va, pm->pm_l1_pa,
	    VM_PROT_READ | VM_PROT_WRITE, 0);
	pmap_update(pmap_kernel());
	pm->pm_l1 = (pt_entry_t *)va;

	/*
	 * Copy kernel L1 entries (upper half: VA >= 0x80000000).
	 * Entries added after this copy are lazily propagated by
	 * trap.c on TLB miss (see pmap_alloc_l2 invariant).
	 * splhigh prevents an interrupt from observing a half-copied L1.
	 */
	unsigned int kern_start = PT_L1_INDEX(VM_MIN_KERNEL_ADDRESS);
	pt_entry_t *kl1 = kernel_pmap_store.pm_l1;
	int s = splhigh();
	for (unsigned int i = kern_start; i < PT_L1_NENTRIES; i++)
		pm->pm_l1[i] = kl1[i];
	splx(s);

	return pm;
}

/*
 * pmap_destroy: free a user pmap.
 *
 * Drops a reference; when it hits zero, free the L1 table
 * and the pmap struct.  (L2 tables should already be freed
 * by pmap_remove_all.)
 */
void
pmap_destroy(pmap_t pm)
{
	if (--pm->pm_count > 0)
		return;

	/* Free the L1 KVA mapping and page */
	pmap_kremove((vaddr_t)pm->pm_l1, PAGE_SIZE);
	pmap_update(pmap_kernel());
	uvm_km_free(kernel_map, (vaddr_t)pm->pm_l1, PAGE_SIZE,
	    UVM_KMF_VAONLY);
	uvm_pagefree(PHYS_TO_VM_PAGE(pm->pm_l1_pa));

	mutex_destroy(&pm->pm_lock);
	kmem_free(pm, sizeof(*pm));
}

void
pmap_reference(pmap_t pm)
{
	pm->pm_count++;
}

/*
 * pmap_enter: create or update a mapping in a pmap.
 *
 * Called by uvm_fault (demand paging), mmap, etc.
 * For kernel pmap: global, no PTE_U.
 * For user pmap: PTE_U set, PTE_SW_MANAGED for UVM-owned pages.
 *
 * flags contains PMAP_WIRED if the mapping should be wired,
 * and may OR in access type (VM_PROT_*) for initial fault info.
 */
int
pmap_enter(pmap_t pmap, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	bool is_kernel = (pmap == pmap_kernel());
	uint32_t extra = is_kernel ? PTE_G : PTE_U;
	pt_entry_t *l1 = pmap->pm_l1;
	unsigned int l1_idx = PT_L1_INDEX(va);

	/*
	 * Single PHYS_TO_VM_PAGE lookup, reused everywhere we'd
	 * otherwise call uvm_pageismanaged(pa) or PHYS_TO_VM_PAGE(pa).
	 * Each of those walks the physseg array; one cached lookup
	 * replaces four function calls per pmap_enter.
	 */
	struct vm_page * const pg =
	    pmap_initialized ? PHYS_TO_VM_PAGE(pa) : NULL;
	const bool managed = (pg != NULL);

	if (managed)
		extra |= PTE_SW_MANAGED;

	const pt_entry_t new_pte = PTE_MAKE(pa, prot, flags, extra);

	/*
	 * Fast path: same-PA remap.  Triggers on NEEDSCOPY
	 * promote-in-place COW upgrades (the parent's first write to
	 * a writable post-fork page, when no copy is needed because
	 * the amap is already exclusively owned) and on wire/protection
	 * adjustments via mprotect/mlock that don't change the PA.
	 *
	 * If the existing PTE already maps `pa`, the PV list is correct
	 * as-is — we can skip pool_get entirely and just update the PTE
	 * bits.  Mirrors sh3's __pmap_map_change.
	 */
	int s = splhigh();
	if (l1[l1_idx] & PTE_V) {
		pt_entry_t *ptep = pmap_pte_lookup(l1, va);
		pt_entry_t old_pte = *ptep;
		if ((old_pte & PTE_V) &&
		    (old_pte & PTE_PPN_MASK) == (pa & PTE_PPN_MASK)) {
			/*
			 * Same PA at same VA: permission / flag change only.
			 * PV list is unchanged.  Resident count is unchanged
			 * (old PTE was already V).
			 */
			*ptep = new_pte;

			if (flags & PMAP_WIRED)
				pmap->pm_stats_wired++;

			if (managed) {
				struct vm_page_md *md = VM_PAGE_TO_MD(pg);
				md->pvh_attrs |= PMAP_MD_REFERENCED;
				if (prot & VM_PROT_WRITE)
					md->pvh_attrs |= PMAP_MD_MODIFIED;
			}

			pmap_tlb_invalidate(pmap, va);

			if (prot & VM_PROT_EXECUTE)
				icache_invalidate();

			splx(s);
			return 0;
		}
	}
	splx(s);

	/*
	 * Slow path: PV list update needed.  Pre-allocate the PV entry
	 * outside splhigh — pool_get with PR_NOWAIT may fail inside
	 * splhigh if the pool needs to grow.
	 */
	struct pv_entry *new_pv = NULL;
	if (managed) {
		new_pv = pool_get(&pv_pool, PR_NOWAIT);
		if (new_pv == NULL)
			return ENOMEM;
	}

	s = splhigh();

	if (!(l1[l1_idx] & PTE_V)) {
		if (!pmap_alloc_l2(l1, l1_idx)) {
			splx(s);
			if (new_pv != NULL)
				pool_put(&pv_pool, new_pv);
			return ENOMEM;
		}
	}

	pt_entry_t *ptep = pmap_pte_lookup(l1, va);
	pt_entry_t old_pte = *ptep;
	*ptep = new_pte;

	if (!(old_pte & PTE_V))
		pmap->pm_stats_resident++;

	if (flags & PMAP_WIRED)
		pmap->pm_stats_wired++;

	/*
	 * PV cleanup for old mapping.  The fast-path same-PA case is
	 * already handled above, but a race window exists: between
	 * fast-path's splx and the slow-path's re-acquired splhigh,
	 * another path could have installed a same-PA mapping.  Detect
	 * that and put the unused new_pv back, the same way the
	 * pre-refactor code did.
	 */
	if ((old_pte & PTE_V) && (old_pte & PTE_SW_MANAGED)) {
		paddr_t old_pa = old_pte & PTE_PPN_MASK;
		if (managed && old_pa == (pa & PTE_PPN_MASK)) {
			pool_put(&pv_pool, new_pv);
			new_pv = NULL;
		} else {
			struct vm_page *old_pg = PHYS_TO_VM_PAGE(old_pa);
			if (old_pg != NULL)
				pv_remove(old_pg, pmap, va);
		}
	}

	if (new_pv != NULL)
		pv_enter(pg, new_pv, pmap, va);

	if (managed) {
		struct vm_page_md *md = VM_PAGE_TO_MD(pg);
		md->pvh_attrs |= PMAP_MD_REFERENCED;
		if (prot & VM_PROT_WRITE)
			md->pvh_attrs |= PMAP_MD_MODIFIED;
	}

	pmap_tlb_invalidate(pmap, va);

	if (prot & VM_PROT_EXECUTE)
		icache_invalidate();

	splx(s);

	return 0;
}

/*
 * pmap_remove: remove mappings in a VA range.
 *
 * Walks L1→L2 page table, clears valid PTEs, and invalidates
 * the TLB for each removed page.
 */
void
pmap_remove(pmap_t pm, vaddr_t sva, vaddr_t eva)
{
	int s = splhigh();
	for (vaddr_t va = sva; va < eva; va += PAGE_SIZE) {
		pt_entry_t *ptep = pmap_pte_lookup(pm->pm_l1, va);
		if (ptep == NULL || !(*ptep & PTE_V))
			continue;

		pt_entry_t pte = *ptep;

		/* Remove PV entry for managed pages */
		if (pte & PTE_SW_MANAGED) {
			struct vm_page *pg =
			    PHYS_TO_VM_PAGE(pte & PTE_PPN_MASK);
			if (pg != NULL)
				pv_remove(pg, pm, va);
		}
		*ptep = 0;
		pm->pm_stats_resident--;
		pmap_tlb_invalidate(pm, va);
	}
	splx(s);
}

/*
 * pmap_protect: change protection on a VA range.
 *
 * NetBSD contract: only removes permissions, never adds.
 * If new protection is VM_PROT_NONE, removes the mapping entirely.
 * Otherwise, masks the PTE permission bits down to match prot.
 */
void
pmap_protect(pmap_t pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{

	/* VM_PROT_NONE → remove entirely */
	if ((prot & VM_PROT_READ) == 0) {
		pmap_remove(pm, sva, eva);
		return;
	}

	pt_entry_t keep = PTE_PROT_BITS(prot);

	int s = splhigh();
	for (vaddr_t va = sva; va < eva; va += PAGE_SIZE) {
		pt_entry_t *ptep = pmap_pte_lookup(pm->pm_l1, va);
		if (ptep == NULL || !(*ptep & PTE_V))
			continue;

		*ptep = (*ptep & ~PTE_PROT_BITS(VM_PROT_ALL)) | keep;
		pmap_tlb_invalidate(pm, va);
	}
	splx(s);
}

/*
 * pmap_unwire: clear wired attribute on a mapping.
 *
 * We don't currently track wired state in PTEs (no software
 * wired bit), so this is a no-op.  Wired mappings are managed
 * by UVM's bookkeeping, not by PTE bits.
 */
void
pmap_unwire(pmap_t pm, vaddr_t va)
{
	/* nothing — no PTE wired bit to clear */
}

/*
 * pmap_extract: look up the physical address for a VA.
 */
bool
pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap)
{
	int s = splhigh();
	pt_entry_t *ptep = pmap_pte_lookup(pm->pm_l1, va);
	pt_entry_t pte = ptep ? *ptep : 0;
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

	if (!(l1[l1_idx] & PTE_V)) {
		if (!pmap_alloc_l2(l1, l1_idx))
			panic("pmap_kenter_pa: L2 alloc failed for va=0x%x",
			    (unsigned)va);
	}

	pt_entry_t *ptep = pmap_pte_lookup(l1, va);
	*ptep = PTE_MAKE(pa, prot, flags, PTE_G);
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
		pt_entry_t *ptep = pmap_pte_lookup(l1, va);
		if (ptep != NULL && (*ptep & PTE_V)) {
			*ptep = 0;
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
 * Pin an L1 page table in TLB slot 1.
 * PT_L1_VA (0xFFFFE000) always maps the active L1.
 * VPN = 0xFFFFE → TLB_VPN = 0x0FFFFE00.
 */
static inline void
pmap_pin_l1(paddr_t l1_pa)
{
	uint32_t idx = PTLB_L1;
	uint32_t vpn = 0x0FFFFE00;
	uint32_t pte = (l1_pa & PTE_PPN_MASK) | PTE_KERNEL;

	__asm__ volatile(
		"WRSYS %0, %1, %2" : : "r"(idx),
		    "n"(SYSDEV_MMU), "n"(MMU_TLB_INDEX));
	__asm__ volatile(
		"WRSYS %0, %1, %2" : : "r"(vpn),
		    "n"(SYSDEV_MMU), "n"(MMU_TLB_VPN));
	__asm__ volatile(
		"WRSYS %0, %1, %2" : : "r"(pte),
		    "n"(SYSDEV_MMU), "n"(MMU_TLB_PTE) : "memory");
}

/*
 * pmap_activate / pmap_deactivate: switch active pmap.
 *
 * Called on every context switch.  Re-pins the L1 page table and
 * sets MMUCR.ASID so the TLB miss handler installs entries tagged
 * with the correct address space.  If the pmap's ASID is stale
 * (generation mismatch), allocates a fresh one first.
 */
void
pmap_activate(struct lwp *l)
{
	struct pmap *pm = l->l_proc->p_vmspace->vm_map.pmap;
	int s = splhigh();

	if (pm == pmap_kernel()) {
		pmap_pin_l1(pm->pm_l1_pa);
		pmap_set_mmucr(0);
		splx(s);
		return;
	}

	/* Allocate a fresh ASID if this pmap's is stale */
	if (pm->pm_asid_gen != pmap_asid_generation)
		pmap_asid_alloc(pm);

	pmap_pin_l1(pm->pm_l1_pa);
	pmap_set_mmucr(pm->pm_asid);
	splx(s);
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
	struct vm_page_md *md = VM_PAGE_TO_MD(pg);
	struct pv_entry *pv;

	if ((prot & VM_PROT_READ) == 0) {
		/*
		 * Remove all mappings to this page.
		 * Delegate to pmap_remove() which handles PTE clear,
		 * PV removal, stats, and TLB invalidation.  Each call
		 * does its own splhigh()/splx(), allowing interrupts
		 * between pages when tearing down large mappings.
		 */
		while ((pv = SLIST_FIRST(&md->pvh_list)) != NULL)
			pmap_remove(pv->pv_pmap, pv->pv_va,
			    pv->pv_va + PAGE_SIZE);
	} else {
		/*
		 * Downgrade: remove permissions not in prot (never adds).
		 * Delegate to pmap_protect() per PV entry.
		 */
		SLIST_FOREACH(pv, &md->pvh_list, pv_link)
			pmap_protect(pv->pv_pmap, pv->pv_va,
			    pv->pv_va + PAGE_SIZE, prot);
	}
}

/*
 * pmap_clear_modify / pmap_clear_reference / pmap_is_modified / pmap_is_referenced:
 * Dirty/reference tracking via software PTE bits.
 */
bool
pmap_clear_modify(struct vm_page *pg)
{
	struct vm_page_md *md = VM_PAGE_TO_MD(pg);
	bool was = (md->pvh_attrs & PMAP_MD_MODIFIED) != 0;

	md->pvh_attrs &= ~PMAP_MD_MODIFIED;

	/*
	 * Remove write permission from all PTEs mapping this page.
	 * This ensures the next write faults, re-enters pmap_enter()
	 * with VM_PROT_WRITE, and re-sets PMAP_MD_MODIFIED.
	 * Without this, UVM could skip writeback of a dirty page.
	 */
	struct pv_entry *pv;
	int s = splhigh();
	SLIST_FOREACH(pv, &md->pvh_list, pv_link) {
		pt_entry_t *ptep = pmap_pte_lookup(pv->pv_pmap->pm_l1,
		    pv->pv_va);
		if (ptep != NULL && (*ptep & PTE_V) && (*ptep & PTE_W)) {
			*ptep &= ~PTE_W;
			pmap_tlb_invalidate(pv->pv_pmap, pv->pv_va);
		}
	}
	splx(s);

	return was;
}

bool
pmap_clear_reference(struct vm_page *pg)
{
	struct vm_page_md *md = VM_PAGE_TO_MD(pg);
	bool was = (md->pvh_attrs & PMAP_MD_REFERENCED) != 0;

	/*
	 * Clear the software reference flag.  Could also strip all
	 * PTE permissions (like pmap_clear_modify strips PTE_W) so
	 * the next access faults and re-sets PMAP_MD_REFERENCED —
	 * but one extra trap per page per page-daemon sweep is
	 * expensive for a 12.5 MHz CPU.  For now just clear the
	 * flag; it gets re-set on the next pmap_enter().
	 */
	md->pvh_attrs &= ~PMAP_MD_REFERENCED;

	return was;
}

bool
pmap_is_modified(struct vm_page *pg)
{
	return (VM_PAGE_TO_MD(pg)->pvh_attrs & PMAP_MD_MODIFIED) != 0;
}

bool
pmap_is_referenced(struct vm_page *pg)
{
	return (VM_PAGE_TO_MD(pg)->pvh_attrs & PMAP_MD_REFERENCED) != 0;
}

/*
 * pmap_procwr: synchronize I-cache after writing to executable pages.
 *
 * Called by MI code after ptrace writes, exec segment loads, etc.
 * With write-through D-cache, the data is already in memory —
 * just invalidate the I-cache.  Hardware only supports full flush.
 */
void
pmap_procwr(struct proc *p, vaddr_t va, size_t len)
{
	icache_invalidate();
}

/*
 * TLB invalidation is implemented in locore.S (WRSYS/RDSYS).
 * Declarations: tlb_invalidate_all/asid/addr in pmap.h.
 */

/*
 * pmap_remove_all — remove all user mappings from a pmap.
 *
 * Called by UVM on process exit, before pmap_destroy().
 * Walks the user half of L1, removes PV entries for managed pages,
 * frees L2 table pages, and bulk-invalidates TLB entries by ASID.
 */
bool
pmap_remove_all(struct pmap *pmap)
{

	if (pmap == pmap_kernel())
		return false;

	unsigned int kern_start = PT_L1_INDEX(VM_MIN_KERNEL_ADDRESS);
	pt_entry_t *l1 = pmap->pm_l1;

	int s = splhigh();

	/* Bulk-invalidate all TLB entries for this ASID */
	if (pmap->pm_asid_gen == pmap_asid_generation)
		tlb_invalidate_asid(pmap->pm_asid);

	/* Walk user half of L1 */
	for (unsigned int i = 0; i < kern_start; i++) {
		if (!(l1[i] & PTE_V))
			continue;

		paddr_t l2_pa = l1[i] & PTE_PPN_MASK;
		pt_entry_t *l2 = pmap_l2_map(l2_pa);

		/* Remove PV entries for managed pages */
		for (unsigned int j = 0; j < PT_L2_NENTRIES; j++) {
			if (!(l2[j] & PTE_V))
				continue;
			if (l2[j] & PTE_SW_MANAGED) {
				vaddr_t va = ((vaddr_t)i << PT_L1_SHIFT) |
				    ((vaddr_t)j << PT_L2_SHIFT);
				struct vm_page *pg =
				    PHYS_TO_VM_PAGE(l2[j] & PTE_PPN_MASK);
				if (pg != NULL)
					pv_remove(pg, pmap, va);
			}
			pmap->pm_stats_resident--;
		}

		/* Free L2 table page and clear L1 entry */
		struct vm_page *l2pg = PHYS_TO_VM_PAGE(l2_pa);
		if (l2pg != NULL)
			uvm_pagefree(l2pg);
		l1[i] = 0;
	}

	splx(s);
	return true;
}

/*
 * pmap_phys_address — convert page frame to physical address.
 */
paddr_t
pmap_phys_address(paddr_t frame)
{
	return ptoa(frame);
}

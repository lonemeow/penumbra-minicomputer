/*	$NetBSD$	*/

/*
 * Penumbra physical map (pmap) — software TLB management.
 *
 * The Penumbra MMU has a 64-entry, 2-way set-associative,
 * fully software-managed TLB.  The OS controls all TLB loads,
 * evictions, and invalidations via WRSYS/RDSYS instructions.
 *
 * Page table layout:
 *   - Processes start with a flat single-level page table
 *     covering 0 – 64 MB (16K entries, 64 KB).
 *   - If VA usage exceeds PENUMBRA_PT1_LIMIT, pmap promotes
 *     to a 2-level page table (1K directory → 1K tables).
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

/* Kernel pmap */
struct pmap kernel_pmap_store;

/* Virtual address range available for kernel VM allocation */
static vaddr_t virtual_avail;
static vaddr_t virtual_end;

/*
 * pmap_bootstrap: early pmap initialization.
 * Called from penumbra_init() before main().
 * At this point, the bootloader has set up enough TLB entries
 * to run the kernel text/data/bss.
 */
void
pmap_bootstrap(void)
{

	/* Initialize kernel pmap */
	mutex_init(&kernel_pmap_store.pm_lock, MUTEX_DEFAULT, IPL_VM);
	kernel_pmap_store.pm_asid = 0;	/* kernel uses ASID 0 + G=1 */
	kernel_pmap_store.pm_level = 1;
	kernel_pmap_store.pm_count = 1;

	/*
	 * Set up kernel virtual address space.
	 * virtual_avail starts after kernel BSS (rounded up to page).
	 * virtual_end is the top of kernel VA.
	 */
	virtual_avail = round_page((vaddr_t)&_end);
	virtual_end = VM_MAX_KERNEL_ADDRESS;
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
 * pmap_init: called after UVM is operational.
 * Allocate pools for pmap structures, PTEs, etc.
 */
void
pmap_init(void)
{
	/* TODO: initialize PTE pools, ASID allocator */
}

/*
 * pmap_create: create a new user pmap.
 */
pmap_t
pmap_create(void)
{
	/* TODO: allocate pmap + flat page table */
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
 * pmap_enter: create a mapping.
 */
int
pmap_enter(pmap_t pm, vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	/* TODO: install PTE, shoot down stale TLB entry */
	return 0;
}

/*
 * pmap_remove: remove mappings in range.
 */
void
pmap_remove(pmap_t pm, vaddr_t sva, vaddr_t eva)
{
	/* TODO */
}

/*
 * pmap_protect: change protection on a range.
 */
void
pmap_protect(pmap_t pm, vaddr_t sva, vaddr_t eva, vm_prot_t prot)
{
	/* TODO */
}

/*
 * pmap_unwire: clear wired attribute.
 */
void
pmap_unwire(pmap_t pm, vaddr_t va)
{
	/* TODO */
}

/*
 * pmap_extract: look up the physical address for a VA.
 */
bool
pmap_extract(pmap_t pm, vaddr_t va, paddr_t *pap)
{
	/* TODO */
	return false;
}

/*
 * pmap_kenter_pa / pmap_kremove: wired kernel mappings.
 */
void
pmap_kenter_pa(vaddr_t va, paddr_t pa, vm_prot_t prot, u_int flags)
{
	/* TODO */
}

void
pmap_kremove(vaddr_t va, vsize_t len)
{
	/* TODO */
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
	/* TODO: load ASID into MMUCR */
}

void
pmap_deactivate(struct lwp *l)
{
	/* nothing needed */
}

/*
 * pmap_zero_page / pmap_copy_page: page operations.
 */
void
pmap_zero_page(paddr_t pa)
{
	/* TODO: temporary map + memset */
}

void
pmap_copy_page(paddr_t src, paddr_t dst)
{
	/* TODO: temporary map + memcpy */
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
 * TLB operations.
 */
void
tlb_invalidate_all(void)
{
	/* TODO: walk all 64 TLB slots, clear V bit */
}

void
tlb_invalidate_asid(int asid)
{
	/* TODO: walk TLB, clear entries matching ASID */
}

void
tlb_invalidate_addr(vaddr_t va, int asid)
{
	/* TODO: clear the specific TLB entry for va/asid */
}

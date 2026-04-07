/*	$NetBSD$	*/

#ifndef _PENUMBRA_PMAP_H_
#define _PENUMBRA_PMAP_H_

/*
 * PTE flag bits (must match hardware TLB_PTE layout).
 * These are pure numeric defines, safe for assembly inclusion.
 */
#define PTE_V		0x01	/* valid */
#define PTE_C		0x04	/* cacheable */
#define PTE_R		0x08	/* read */
#define PTE_W		0x10	/* write */
#define PTE_X		0x20	/* execute */
#define PTE_U		0x40	/* user-accessible */
#define PTE_G		0x80	/* global (skip ASID match) */

/* Software PTE bits (bits 11:8, hardware stores but ignores) */
#define PTE_SW_DIRTY	0x100	/* page has been modified */
#define PTE_SW_REF	0x200	/* page has been referenced */
#define PTE_SW_MANAGED	0x400	/* page is managed by UVM */

#define PTE_PPN_SHIFT	12
#define PTE_PPN_MASK	0xFFFFF000

/* Standard kernel PTE: valid, cached, read, write, execute, global */
#define PTE_KERNEL	(PTE_V | PTE_C | PTE_R | PTE_W | PTE_X | PTE_G)
/* Uncached kernel PTE: for MMIO */
#define PTE_KERNEL_NC	(PTE_V | PTE_R | PTE_W | PTE_G)

/*
 * Two-level page table geometry.
 *
 * VA: [L1 index (10)][L2 index (10)][offset (12)]
 *      bits 31:22     bits 21:12     bits 11:0
 *
 * L1: 1024 entries × 4 bytes = 4 KB (one page).
 * L2: 1024 entries × 4 bytes = 4 KB (one page, covers 4 MB).
 *
 * L1 entries use the same layout as PTEs: PPN points to
 * the L2 page's physical address.  Only the V bit is checked by
 * the TLB miss handler; other flag bits are available for software.
 */
#define PT_L1_SHIFT	22
#define PT_L1_NENTRIES	1024
#define PT_L1_INDEX(va)	(((vaddr_t)(va)) >> PT_L1_SHIFT)
#define PT_L2_SHIFT	12
#define PT_L2_NENTRIES	1024
#define PT_L2_INDEX(va)	((((vaddr_t)(va)) >> PT_L2_SHIFT) & (PT_L2_NENTRIES - 1))
#define PT_L2_COVERAGE	(PT_L2_NENTRIES * PAGE_SIZE)	/* 4 MB per L2 table */

/* Build an L1 entry from an L2 table's physical address */
#define PT_L1E_MAKE(l2pa)	(((l2pa) & PTE_PPN_MASK) | PTE_V)

/*
 * Build a PTE from physical address, NetBSD protection, pmap flags,
 * and caller-supplied extra PTE bits (e.g., PTE_G, PTE_U, PTE_SW_WIRED).
 *
 * prot:  VM_PROT_READ / VM_PROT_WRITE / VM_PROT_EXECUTE
 * flags: PMAP_NOCACHE to disable caching (MMIO)
 * extra: PTE bits the caller always wants set (PTE_G for kernel, etc.)
 */
#define PTE_PROT_BITS(prot)                             \
	((((prot) & VM_PROT_READ)      ? PTE_R : 0)			\
	 | (((prot) & VM_PROT_WRITE)   ? PTE_W : 0)			\
	 | (((prot) & VM_PROT_EXECUTE) ? PTE_X : 0))

#define PTE_MAKE(pa, prot, flags, extra)				\
	(((pa) & PTE_PPN_MASK) | PTE_V						\
	 | PTE_PROT_BITS(prot)                              \
	 | (((flags) & PMAP_NOCACHE)   ? 0     : PTE_C)		\
	 | (extra))

/*
 * Fixed kernel VAs for pinned TLB slots.
 * Last page (0xFFFFF000) is an unmapped guard to catch -1 derefs.
 * VA 0x0 is NOT mapped — it is the user null guard page.
 */
#define PT_L1_VA	0xFFFFE000	/* pinned slot 1: current L1 table */
#define PT_L2WIN_VA	0xFFFFD000	/* pinned slot 2: L2 window (handler) */
#define SCRATCH_VA	0xFFFFC000	/* pinned slot 3: C scratch window */
#define VECTOR_VA	0xFFFFB000	/* pinned slot 0: vector/handler page */

/*
 * Number of pre-allocated L2 tables in BSS (locore.S).
 * Must cover the kernel image (KERNEL_TEXT_BASE through BSS+stack).
 * 3 tables = 12 MB, enough for any reasonable kernel.
 */
#define BOOT_NL2	3

#if defined(_KERNEL) && !defined(_LOCORE)

#include <sys/mutex.h>
#include <machine/vmparam.h>
#include <machine/types.h>

/*
 * Penumbra page table entry.
 * Matches TLB_PTE hardware format: PPN[31:12] | SW[11:8] | flags[7:0]
 */
typedef uint32_t pt_entry_t;

/*
 * Page table structure — always 2-level (L1 + L2 tables).
 *
 * The TLB miss handler is context-blind: it always walks a 2-level
 * table via the pinned L1.  Universal 2-level keeps the handler
 * stateless — no stack, no curproc, no metadata access needed.
 */
struct pmap {
	kmutex_t	pm_lock;
	pt_entry_t	*pm_l1;	/* L1 table kernel VA */
	paddr_t		pm_l1_pa;	/* L1 table physical address (for TLB pin) */
	int		pm_asid;	/* address space ID (0-255) */
	int		pm_count;	/* reference count */
	int		pm_stats_resident;
	int		pm_stats_wired;
};

/* pmap_t and pmap_kernel() are defined by <uvm/uvm_pmap.h> */

extern struct pmap kernel_pmap_store;
#define pmap_resident_count(pm)	((pm)->pm_stats_resident)
#define pmap_wired_count(pm)	((pm)->pm_stats_wired)

/* Stub out pmap_update for now — TLB is software-managed */
#define pmap_update(pm)		((void)0)

/* UVM calls pmap_steal_memory() for early page allocation */
#define PMAP_STEAL_MEMORY

/*
 * Scratch window: temporarily map a physical page at SCRATCH_VA
 * via pinned TLB slot 3.  Used by pmap_steal_memory (to write to
 * freshly allocated pages that have no VA yet), pmap_zero_page,
 * pmap_copy_page, and L2 table allocation.
 *
 * Uniprocessor, interrupts disabled during early boot — no
 * concurrency issues.  The mapping persists until overwritten.
 */
void		pmap_scratch_map(paddr_t pa, pt_entry_t flags);
void		*pmap_scratch_va(void);

/* phys_bias: set by locore.S, used only during bootstrap */
extern int32_t phys_bias;

/* BSS page tables allocated by locore.S */
extern char _boot_l1[];
extern char _boot_l2[];

/* Required pmap interface — implemented in pmap.c */
void		pmap_bootstrap(void);
void		pmap_virtual_space(vaddr_t *, vaddr_t *);
vaddr_t		pmap_steal_memory(vsize_t, vaddr_t *, vaddr_t *);
paddr_t		pmap_steal_page(void);
vaddr_t		pmap_map_device(paddr_t, vsize_t);

/* TLB operations — implemented in locore.S */
void		tlb_invalidate_all(void);
void		tlb_invalidate_asid(int);
void		tlb_invalidate_addr(vaddr_t, int);

#endif /* _KERNEL && !_LOCORE */

#endif /* _PENUMBRA_PMAP_H_ */

/*	$NetBSD$	*/

#ifndef _PENUMBRA_PMAP_H_
#define _PENUMBRA_PMAP_H_

#include <sys/mutex.h>
#include <machine/vmparam.h>
#include <machine/types.h>

/*
 * Penumbra page table entry.
 * Matches TLB_PTE hardware format: PPN[31:12] | SW[11:8] | flags[7:0]
 */
typedef uint32_t pt_entry_t;

/* PTE flag bits (must match hardware TLB_PTE layout) */
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
#define PTE_SW_WIRED	0x400	/* page is wired */
#define PTE_SW_MANAGED	0x800	/* page is managed by UVM */

#define PTE_PPN_SHIFT	12
#define PTE_PPN_MASK	0xFFFFF000

/* Standard kernel PTE: valid, cached, read, write, execute, global */
#define PTE_KERNEL	(PTE_V | PTE_C | PTE_R | PTE_W | PTE_X | PTE_G)
/* Uncached kernel PTE: for MMIO */
#define PTE_KERNEL_NC	(PTE_V | PTE_R | PTE_W | PTE_G)

#ifdef _KERNEL

/*
 * Page table structure.
 * Starts as flat single-level for VA < PENUMBRA_PT1_LIMIT (64 MB).
 * Promoted to 2-level if process uses VA above that threshold.
 */
#define PENUMBRA_PT1_NENTRIES	(PENUMBRA_PT1_LIMIT / PAGE_SIZE)

struct pmap {
	kmutex_t	pm_lock;
	pt_entry_t	*pm_pt;		/* page table (flat or L2 directory) */
	int		pm_asid;	/* address space ID (0-255) */
	int		pm_level;	/* 1 = flat, 2 = two-level */
	int		pm_count;	/* reference count */
	int		pm_stats_resident;
	int		pm_stats_wired;
};

typedef struct pmap *pmap_t;

extern struct pmap kernel_pmap_store;
#define pmap_kernel()		(&kernel_pmap_store)
#define pmap_resident_count(pm)	((pm)->pm_stats_resident)
#define pmap_wired_count(pm)	((pm)->pm_stats_wired)

/* Stub out pmap_update for now — TLB is software-managed */
#define pmap_update(pm)		((void)0)

/* Required pmap interface — implemented in pmap.c */
void		pmap_bootstrap(void);
void		pmap_virtual_space(vaddr_t *, vaddr_t *);

/* TLB operations */
void		tlb_invalidate_all(void);
void		tlb_invalidate_asid(int);
void		tlb_invalidate_addr(vaddr_t, int);

#endif /* _KERNEL */

#endif /* _PENUMBRA_PMAP_H_ */

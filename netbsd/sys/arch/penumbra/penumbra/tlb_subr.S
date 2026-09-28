/*	$NetBSD$	*/

/*
 * Main-TLB invalidation, called from C (pmap.c).
 *
 * The routines reach the TLB only through the MMU sysregs, so they
 * depend on nothing else in the kernel and can be assembled and tested
 * on their own (make test-kernel).
 */

#include <machine/sysreg.h>
#include <machine/pmap.h>

	.text

/*
 * tlb_invalidate_all: invalidate all 64 main TLB slots.
 * Does NOT touch pinned TLB entries.
 */
	.globl	tlb_invalidate_all
	.type	tlb_invalidate_all, @function
tlb_invalidate_all:
	mov	r1, r0			/* index = 0 */
.Ltia_loop:
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE	/* V=0 → invalid */
	add	r1, #1
	cmp	r1, #TLB_NSLOTS
	blo	.Ltia_loop
	ret

/*
 * tlb_invalidate_asid(int asid): invalidate all TLB entries
 * matching the given ASID.  Skips global (G=1) entries.
 */
	.globl	tlb_invalidate_asid
	.type	tlb_invalidate_asid, @function
tlb_invalidate_asid:
	/* R1 = asid */
	stw	r5, [sp + #-4]
	stw	r6, [sp + #-8]
	sub	sp, #8
	mov	r5, r1			/* R5 = target ASID */
	mov	r2, r0			/* R2 = slot index */
.Ltias_loop:
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_INDEX
	rdsys	r3, #SYSDEV_MMU, #MMU_TLB_PTE
	mov	r4, r3
	and	r4, #PTE_V
	beq	.Ltias_next		/* skip invalid entries */
	mov	r4, r3
	and	r4, #PTE_G
	bne	.Ltias_next		/* skip global entries */
	rdsys	r3, #SYSDEV_MMU, #MMU_TLB_VPN
	mov	r6, r3
	and	r6, #0xFF		/* extract ASID from TLB_VPN */
	cmp	r6, r5
	bne	.Ltias_next
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE	/* invalidate */
.Ltias_next:
	add	r2, #1
	cmp	r2, #TLB_NSLOTS
	blo	.Ltias_loop
	add	sp, #8
	ldw	r5, [sp + #-4]
	ldw	r6, [sp + #-8]
	ret

/*
 * tlb_invalidate_addr(vaddr_t va, int asid): invalidate TLB
 * entry for a specific VA + ASID.  Checks both ways of the set.
 *
 * Works correctly for G=1 entries because the TLB miss handler
 * always installs G=1 entries with ASID=0, and kernel code
 * always calls tlb_invalidate_addr(va, 0) for kernel VAs.
 */
	.globl	tlb_invalidate_addr
	.type	tlb_invalidate_addr, @function
tlb_invalidate_addr:
	/* R1 = va, R2 = asid */
	/* Compute expected TLB_VPN = (VPN << 8) | ASID */
	mov	r3, r1
	shr	r3, #12
	shl	r3, #8
	or	r3, r2			/* R3 = expected TLB_VPN */

	/* Compute set = VPN[4:0] */
	mov	r4, r1
	shr	r4, #12
	and	r4, #0x1F		/* R4 = set index */

	/* Check way 0 */
	wrsys	r4, #SYSDEV_MMU, #MMU_TLB_INDEX
	rdsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN
	cmp	r1, r3
	bne	.Ltia_w1
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE	/* invalidate way 0 */
	ret

.Ltia_w1:
	/* Check way 1 */
	mov	r1, r4
	add	r1, #0x20		/* way 1 = set + 0x20 (bit 5) */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	rdsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN
	cmp	r1, r3
	bne	.Ltia_done
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE	/* invalidate way 1 */
.Ltia_done:
	ret

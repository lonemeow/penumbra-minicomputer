/*	$NetBSD$	*/

/*
 * The TLB refill handler, for one MMU.  locore.S defines RMH_MMUCFG to
 * an MMU_CONFIG value the kernel drives and includes this file where
 * that MMU's copy of the handler belongs; it is never assembled on its
 * own.
 *
 * The handler walks the 2-level page table (L1 -> L2) through pinned
 * TLB slots:
 *   Slot 1: Kernel L1   at PT_KERN_L1_VA (0xFFFFE000, permanent)
 *   Slot 2: L2 window   at PT_L2WIN_VA   (0xFFFFD000, remapped per miss)
 *   Slot 4: User L1     at PT_USER_L1_VA (0xFFFFA000, re-pinned by
 *                                          pmap_activate per LWP)
 * and branches on the faulting VA's MSB: kernel VAs (signed-negative)
 * walk the kernel L1 via slot 1, user VAs the current user L1 via slot
 * 4 (SH-4-style split walker).  Kernel and user L1s are disjoint, so
 * user pmaps need no kernel-half mirroring.
 *
 * The geometry is decoded here into the immediates the handler runs
 * on, since a constant in a register would cost a load on the kernel's
 * hottest path (doc/system/netbsd/design-notes.md, "Trap page: code
 * variants, not parameters").  An MMU_CONFIG value the handler cannot
 * serve fails the build.
 *
 * Each copy fills RMH_SPACE bytes, the space locore.S reserves for a
 * refill handler at the head of the trap page, padded behind its last
 * instruction; one that does not fit fails the build.  The trap entry
 * stubs are linked behind that space, so the fault path reaches them
 * by a branch relative to the handler's own start, which holds wherever
 * the copy runs.  The labels are numeric so that each copy has its own.
 */

#ifndef RMH_MMUCFG
#error "RMH_MMUCFG: define the MMU_CONFIG value to build the handler for"
#endif
#ifndef RMH_SPACE
#error "RMH_SPACE: define the bytes reserved for a refill handler"
#endif

#define	RMH_SETS	(1 << MMUCFG_SETS_LOG2(RMH_MMUCFG))
#define	RMH_WAYS	MMUCFG_WAYS(RMH_MMUCFG)

#if MMUCFG_VER(RMH_MMUCFG) == 1
#define	RMH_PINNED	TLB_INDEX_PINNED_V1
#elif MMUCFG_VER(RMH_MMUCFG) == 2
#define	RMH_PINNED	TLB_INDEX_PINNED_V2
#else
#error "RMH_MMUCFG: no TLB_INDEX layout for this interface version"
#endif

#if MMUCFG_PINNED(RMH_MMUCFG) <= PTLB_SLOT_CPU_INFO
#error "RMH_MMUCFG: fewer pinned slots than the kernel pins"
#endif
#if RMH_WAYS == 0 || (RMH_WAYS & (RMH_WAYS - 1)) != 0
#error "RMH_MMUCFG: the way comes from counter bits, so WAYS must be a power of two"
#endif
#if (RMH_SETS - 1) > 0xFFFF || (RMH_WAYS - 1) * RMH_SETS > 0xFFFF
#error "RMH_MMUCFG: the set or way mask is wider than an immediate"
#endif
#if MMUCFG_PAGE_LOG2(RMH_MMUCFG) != PGSHIFT
#error "RMH_MMUCFG: the handler walks PGSHIFT-sized pages"
#endif
#if MMUCFG_ASID_BITS(RMH_MMUCFG) != 8
#error "RMH_MMUCFG: the handler and the ASID allocator use 8-bit ASIDs"
#endif

0:	/* RMH_SPACE is measured from here */
	/* Park R1/R2/R3 in the CPU-internal scratch SPRs (SCR0..2).
	 * These are 32-bit storage cells exposed via WRSPR/RDSPR;
	 * no memory access, no cache or TLB involvement.  See
	 * doc/system/architecture.md "Scratch SPRs (SCR0–SCR3)".
	 *
	 * MMU_FAULT_ADDR is latched, so the faulting VA can be re-read
	 * from the sysreg later if needed — that re-read replaces what
	 * would otherwise be a fourth scratch register.
	 */
	wrspr	scr0, r1
	wrspr	scr1, r2
	wrspr	scr2, r3

	/* Read faulting virtual address */
	rdsys	r1, #SYSDEV_MMU, #MMU_FAULT_ADDR

	/* ── L1 lookup (SH-4-style split walker) ────
	 * R1 = faulting VA
	 * Branch on VA MSB: kernel VAs (>= 0x80000000, signed < 0)
	 * walk PT_KERN_L1_VA (slot 1); user VAs walk PT_USER_L1_VA
	 * (slot 4).  Sign-test via cmp/blt: kernel VAs are signed
	 * negative, user VAs signed non-negative.
	 * L1 index = VA[31:22], each entry is 4 bytes.
	 */
	mov	r2, r1
	shr	r2, #PT_L1_SHIFT		/* R2 = L1 index (bits 31:22) */
	shl	r2, #2			/* R2 = byte offset into L1 */

	cmp	r1, r0			/* signed compare VA to 0 */
	blt	1f			/* VA < 0 (MSB set) → kernel L1 */
	lli	r3, #0xA000		/* user VA → PT_USER_L1_VA */
	b	2f
1:	/* kernel VA */
	lli	r3, #0xE000		/* kernel VA → PT_KERN_L1_VA */
2:	/* R3's low half chosen above */
	lui	r3, #0xFFFF		/* R3 = 0xFFFF_X000 selected above */
	add	r3, r2			/* R3 = &L1[index] */
	ldw	r3, [r3]		/* R3 = L1E (L2 PA | flags) */

	/* Check L1 entry valid bit */
	test	r3, #PTE_V
	beq	4f			/* L1E invalid → page fault */

	/* ── Map L2 table into pinned window (slot 2) ──
	 * Extract L2 table PA from L1E (clear flag bits),
	 * then remap pinned slot 2 to point there.
	 *
	 * Window VA = 0xFFFFD000.  TLB entry:
	 *   TLB_INDEX = pinned select | slot 2
	 *   TLB_VPN  = (0xFFFFD >> 0) << 8 = 0x0FFFFD00
	 *   TLB_PTE  = L2_PA | V|C|R|G (0x8D)
	 */
	shr	r3, #PTE_PPN_SHIFT
	shl	r3, #PTE_PPN_SHIFT	/* R3 = L2 table PA (aligned) */

	lli	r2, #((RMH_PINNED | PTLB_SLOT_L2WIN) & 0xFFFF)
#if (RMH_PINNED >> 16) != 0
	lui	r2, #(RMH_PINNED >> 16)
#endif
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_INDEX

	lli	r2, #0xFD00
	lui	r2, #0x0FFF		/* R2 = 0x0FFFFD00 (TLB_VPN) */
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_VPN

	lli	r2, #0x8D		/* V|C|R|G = 0x01|0x04|0x08|0x80 */
	or	r2, r3			/* R2 = L2_PA | flags */
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_PTE

	/* ── L2 table lookup ─────────────────────────
	 * L2 index = VA[21:12], each entry is 4 bytes.
	 * L2 table is now accessible at VA 0xFFFFD000.
	 */
	mov	r2, r1
	shr	r2, #PT_L2_SHIFT
	and	r2, #0x3FF		/* R2 = L2 index (bits 21:12) */
	shl	r2, #2			/* R2 = byte offset into L2 */

	lli	r3, #0xD000
	lui	r3, #0xFFFF		/* R3 = 0xFFFFD000 (PT_L2WIN_VA) */
	add	r3, r2			/* R3 = &L2[index] */
	ldw	r3, [r3]		/* R3 = PTE value */

	/* Check PTE valid bit (TEST = AND with F=1, no writeback) */
	test	r3, #PTE_V
	beq	4f			/* PTE invalid → page fault */

	/* ── Load PTE into main TLB ──────────────────
	 *
	 * Live state at this point:
	 *   R1 = faulting VA
	 *   R3 = PTE value (must be preserved for the final wrsys)
	 *   R2 = scratch (free)
	 */

	shr	r1, #12
	and	r1, #(RMH_SETS - 1)	/* R1 = set: the VPN's low bits */

	/* Pick the way from the free-running cycle counter, a cheap
	 * stand-in for a random choice: a slot number is way * SETS + set,
	 * so with a power-of-two way count the counter bits just above the
	 * set field are way * SETS as they stand.  TLB hits never trap, so
	 * software has no record of use to replace by. */
	rdsys	r2, #SYSDEV_CPU, #CPU_CYCLES
	and	r2, #((RMH_WAYS - 1) * RMH_SETS)
	or	r1, r2			/* R1 = way * SETS + set */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX

	rdsys	r1, #SYSDEV_MMU, #MMU_FAULT_ADDR
	shr	r1, #12
	shl	r1, #8
	test	r3, #PTE_G
	bne	3f			/* G=1 → ASID=0, already in R1 */
	rdsys	r2, #SYSDEV_MMU, #MMU_MMUCR
	shr	r2, #MMUCR_ASID_SHIFT
	and	r2, #0xFF
	or	r1, r2
3:	/* R1 = TLB_VPN word */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN
	wrsys	r3, #SYSDEV_MMU, #MMU_TLB_PTE

	/* Restore scratch registers and return */
	rdspr	r1, scr0
	rdspr	r2, scr1
	rdspr	r3, scr2
	eret

4:	/* no mapping */
	/*
	 * TLB miss handler failed to find a mapping.
	 * R1-R3 were parked in SCR0..2 on handler entry.
	 * Restore them and enter the common trap path with cause=2
	 * (EXC_TLB_MISS).
	 */
	rdspr	r1, scr0
	rdspr	r2, scr1
	rdspr	r3, scr2
	b	0b + RMH_SPACE		/* _trap_entry_tlbmiss */

	.if (. - 0b) > RMH_SPACE
	.error "refill handler does not fit RMH_SPACE"
	.endif
	.fill	(RMH_SPACE - (. - 0b)) / 4, 4, 0

#undef	RMH_SETS
#undef	RMH_WAYS
#undef	RMH_PINNED

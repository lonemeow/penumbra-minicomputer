/*	$NetBSD$	*/

/*
 * Penumbra low-level kernel entry and exception handling.
 *
 * Entry point: _start (jumped to by bootloader with MMU OFF,
 * R1 = bootinfo physical address, running in supervisor mode).
 *
 * The bootloader loads the kernel ELF at an arbitrary physical
 * address and jumps with:
 *   R1  = bootinfo physical address
 *   R2  = kernel physical entry point
 *   SR.S = 1 (supervisor), SR.I = 0 (irqs off); MMU off (MMUCR.M = 0)
 *
 * The kernel is linked at KERNEL_TEXT_BASE (0x8001_0000).
 * locore.S must: compute virt-to-phys bias, set up the kernel
 * page table (L1 + L2 tables in BSS), install the real
 * page-table-walking TLB miss handler, enable the MMU, and
 * jump to virtual addresses before any C code can run.
 *
 * No bootstrap handler — the real handler is installed from the
 * start.  The page table is built entirely in physical mode.
 *
 * Exception vectors live at physical 0x0000_0000:
 *   0x00: bus fault    0x04: IRQ          0x08: TLB miss
 *   0x0C: TLB prot    0x10: privilege     0x14: SYSCALL
 *   0x18: BREAK        0x1C: illegal      0x20: alignment
 *   0x24: EXT_IRQ      0x28: arith (DIV0)
 */

#include "assym.h"
#include <machine/sysreg.h>
#include <machine/pmap.h>
#include <machine/param.h>

#define KERN_TEXT_VA	0x80010000	/* KERNEL_TEXT_BASE */
#define BOOTINFO_MAGIC	0x50454E49	/* "PENI" — must match bootinfo.h */
#define BOOTINFO_MAXSIZE 4096

	.text
	.globl	_start
	.type	_start, @function

/*
 * Kernel entry point.
 * Called by bootloader with:
 *   R1 = bootinfo physical address
 *   R2 = kernel physical entry point
 *   SR.S = 1 (supervisor), SR.I = 0 (irqs off); MMU off (MMUCR.M = 0)
 *
 * Register allocation for early boot:
 *   R5 = saved bootinfo physical address
 *   R6 = phys_bias (physical - virtual offset)
 */
_start:
	/* Save bootinfo pointer — R1 will be clobbered */
	mov	r5, r1

	/*
	 * Compute phys_bias = physical_addr - virtual_addr.
	 * LA loads the link-time (virtual) address of _start_bias;
	 * mov r6, pc at that label reads the runtime (physical) address.
	 */
	la	r1, _start_bias
_start_bias:
	mov	r6, pc
	sub	r6, r1

	/*
	 * Set up early boot stack (physical address).
	 * The boot stack lives in BSS — physical memory is already
	 * allocated by the ELF loader (p_memsz).  Convert the
	 * link-time virtual address to physical using the bias.
	 * Stack grows downward, so point SP at the top.
	 */
	la	sp, _boot_stack_top
	add	sp, r6

	/* Flush the entire TLB — invalidate all slots */
	mov	r1, r0
.Lflush_tlb:
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE
	add	r1, #1
	cmp	r1, #TLB_NSLOTS
	blo	.Lflush_tlb

	/* Invalidate I-cache — stale bootloader code may be cached */
	wrsys	r0, #SYSDEV_L1_ICACHE, #CACHE_INVAL_ALL

/* ═══════════════════════════════════════════════════════════════
 * Step 4: Zero BSS (physical mode)
 *
 * BSS must be zeroed before any C code runs.  Done here in
 * physical mode so that step 5 can copy bootinfo into a BSS
 * buffer before the MMU transition makes bootloader memory
 * unreachable.  Addresses use PA = VA + phys_bias.
 *
 * This also zeroes the BSS-allocated L1 and L2 tables
 * (_boot_l1, _boot_l2), so they start with all entries invalid.
 * ═══════════════════════════════════════════════════════════════ */

	la	r1, __bss_start
	add	r1, r6			/* PA of __bss_start */
	la	r2, _end
	add	r2, r6			/* PA of _end */
.Lzero_bss:
	cmp	r1, r2
	bhs	.Lbss_done
	stw	r0, [r1]
	add	r1, #4
	b	.Lzero_bss
.Lbss_done:

	/* Export phys_bias to .data for C code */
	la	r1, phys_bias
	add	r1, r6			/* PA of phys_bias */
	stw	r6, [r1]

/* ═══════════════════════════════════════════════════════════════
 * Step 5: Copy bootinfo to kernel BSS (physical mode)
 *
 * R5 = bootinfo physical address (from bootloader).
 * Copy into bootinfo_store[] (BSS, just zeroed) so the kernel
 * can access it after MMU enable.  Read the size field from the
 * bootinfo header; clamp to BOOTINFO_MAXSIZE (4096).
 * Also set penumbra_bootinfo to the virtual address of the store.
 * ═══════════════════════════════════════════════════════════════ */

	/* Validate magic before copying */
	ldw	r1, [r5]		/* bootinfo->magic */
	li	r2, #BOOTINFO_MAGIC
	cmp	r1, r2
	bne	.Lbootinfo_done		/* skip if invalid */

	/* Read size, clamp to BOOTINFO_MAXSIZE */
	ldw	r3, [r5 + #4]		/* bootinfo->size */
	li	r1, #BOOTINFO_MAXSIZE
	cmp	r3, r1
	bls	.Lbi_size_ok
	mov	r3, r1			/* clamp */
.Lbi_size_ok:

	/* Copy r3 bytes from r5 (src PA) to bootinfo_store (dst PA) */
	la	r1, bootinfo_store
	add	r1, r6			/* PA of bootinfo_store */
	mov	r2, r5			/* source = bootinfo phys */
.Lbi_copy:
	cmp	r3, #0
	beq	.Lbi_copy_done
	ldw	r4, [r2]
	stw	r4, [r1]
	add	r1, #4
	add	r2, #4
	sub	r3, #4
	b	.Lbi_copy
.Lbi_copy_done:

	/* Set penumbra_bootinfo = virtual address of bootinfo_store */
	la	r1, penumbra_bootinfo
	add	r1, r6			/* PA of penumbra_bootinfo */
	la	r2, bootinfo_store	/* VA of bootinfo_store */
	stw	r2, [r1]
.Lbootinfo_done:

/* ═══════════════════════════════════════════════════════════════
 * Step 6: Build kernel page table (physical mode)
 *
 * The L1 and L2 tables live in BSS (zeroed in step 4).
 * Populate L1 entries and L2 PTEs for the kernel image
 * (KERNEL_TEXT_BASE through _boot_stack_top).
 *
 * Register allocation:
 *   R5 = saved bootinfo PA (preserved)
 *   R6 = phys_bias (preserved)
 *   R7 = L1 table physical address
 *   R8 = current VA being mapped (page-aligned)
 *   R9 = end VA (page-aligned, exclusive)
 *   R10 = current PA being mapped
 * ═══════════════════════════════════════════════════════════════ */

	/* Compute L1 table PA */
	la	r7, _boot_l1
	add	r7, r6			/* R7 = PA of _boot_l1 */

	/*
	 * Install L1 entries for all BOOT_NL2 L2 tables.
	 * L2 tables are contiguous in BSS: _boot_l2[0], [1], [2].
	 * First L1 index = KERNEL_TEXT_BASE >> 22.
	 * Install BOOT_NL2 consecutive entries starting there,
	 * giving headroom beyond _end for stolen pages and UART.
	 */
	li	r1, #KERN_TEXT_VA
	shr	r1, #PT_L1_SHIFT		/* R1 = first L1 index */

	mov	r2, r1
	add	r2, #(BOOT_NL2 - 1)		/* R2 = last L1 index (inclusive) */

	la	r3, _boot_l2
	add	r3, r6			/* R3 = PA of first L2 table */

	mov	r4, r1			/* R4 = current L1 index */
.Ll1e_loop:
	cmp	r4, r2
	bhi	.Ll1e_done

	/* L1[r4] = L2_PA | PTE_V */
	mov	r8, r4
	shl	r8, #2			/* byte offset in L1 */
	add	r8, r7			/* PA of L1 entry */
	mov	r9, r3
	lli	r10, #PTE_V
	or	r9, r10			/* L1E = L2_PA | V */
	stw	r9, [r8]

	add	r3, #4096		/* next L2 table */
	add	r4, #1
	b	.Ll1e_loop
.Ll1e_done:

	/*
	 * Populate L2 PTEs for kernel image.
	 * Map from KERNEL_TEXT_BASE to round_page(_end).
	 * PTE = PA | PTE_KERNEL (V|C|R|W|X|G = 0xBD).
	 *
	 * Safety check: verify _end fits within BOOT_NL2 L2 tables.
	 * If the kernel outgrows the pre-allocated tables, halt
	 * immediately rather than silently corrupting memory.
	 */
	li	r8, #KERN_TEXT_VA	/* R8 = current VA */

	la	r9, _end
	add	r9, #0xFFF		/* round up to page */
	shr	r9, #12
	shl	r9, #12			/* R9 = end VA (rounded) */

	/* Check: does _end exceed BOOT_NL2 coverage? */
	mov	r1, r9
	shr	r1, #PT_L1_SHIFT	/* L1 index of _end */
	li	r2, #KERN_TEXT_VA
	shr	r2, #PT_L1_SHIFT	/* first L1 index */
	sub	r1, r2			/* number of L2 tables needed - 1 */
	cmp	r1, #BOOT_NL2
	blo	.Lpt_fits
	/* Kernel too large for BOOT_NL2 L2 tables — halt */
	break
.Lpt_fits:

	mov	r10, r8
	add	r10, r6			/* R10 = current PA */

.Lpte_loop:
	cmp	r8, r9
	bhs	.Lpte_done

	/*
	 * Compute L2 table address for this VA.
	 * L1 index = VA >> 22
	 * L2 table = _boot_l2 + (l1_idx - first_l1_idx) * 4096
	 * L2 index = (VA >> 12) & 0x3FF
	 */
	mov	r1, r8
	shr	r1, #PT_L1_SHIFT		/* L1 index */
	li	r2, #KERN_TEXT_VA
	shr	r2, #PT_L1_SHIFT		/* first L1 index */
	sub	r1, r2			/* offset in L2 array */
	shl	r1, #12			/* * 4096 = byte offset */
	la	r2, _boot_l2
	add	r2, r6			/* PA of _boot_l2 base */
	add	r1, r2			/* PA of L2 table for this VA */

	mov	r2, r8
	shr	r2, #PT_L2_SHIFT
	and	r2, #0x3FF		/* L2 index */
	shl	r2, #2			/* byte offset in L2 */
	add	r1, r2			/* PA of PTE slot */

	/* Build PTE = PA | PTE_KERNEL */
	mov	r2, r10
	shr	r2, #PTE_PPN_SHIFT
	shl	r2, #PTE_PPN_SHIFT	/* PPN aligned */
	lli	r3, #PTE_KERNEL
	or	r2, r3			/* PTE value */
	stw	r2, [r1]		/* write PTE */

	add	r8, #4096		/* next VA page */
	add	r10, #4096		/* next PA page */
	b	.Lpte_loop
.Lpte_done:

/* ═══════════════════════════════════════════════════════════════
 * Step 7: Copy real TLB miss handler to vector page
 *
 * The real handler walks L1 → L2 via pinned TLB slots.
 * Copy to vector page offset 0x80 (same location as before).
 *
 * Vector page layout (PA 0x0, mapped at VECTOR_VA):
 *   0x00-0x1F   Exception vectors (8 × 4-byte addresses)
 *   0x80+       TLB miss handler code
 *   (after code) Trap-entry scratch (R1 save, ESR/EPC/fault snapshot)
 *               Accessed via PC-relative — no VA 0 dependency
 * ═══════════════════════════════════════════════════════════════ */

	la	r1, _real_miss_handler
	add	r1, r6			/* physical source address */
	lli	r2, #0x80		/* destination in vector page —
					 * MUST be a multiple of the L1 cache
					 * line size (16) to preserve the
					 * handler's .balign 16 at runtime */
	la	r3, _real_handlers_end
	add	r3, r6			/* physical end of handler code */
.Lcopy_handler:
	ldw	r4, [r1]
	stw	r4, [r2]
	add	r1, #4
	add	r2, #4
	cmp	r1, r3
	blo	.Lcopy_handler

	/* I-cache invalidate after writing code to vector page */
	wrsys	r0, #SYSDEV_L1_ICACHE, #CACHE_INVAL_ALL

/* ═══════════════════════════════════════════════════════════════
 * Step 8: Install exception vectors
 *
 * Vector table entries contain virtual addresses.  After MMU
 * enable, the CPU loads the handler VA from the vector table
 * (physical fetch, MMU bypassed for the table read itself)
 * and jumps to it (MMU translates the handler VA).
 *
 * The handler code is at PA 0x80, mapped via pinned slot 0 at
 * VECTOR_VA (0xFFFFB000).  So handler VA = VECTOR_VA + 0x80.
 * ═══════════════════════════════════════════════════════════════ */

	/*
	 * Install exception vector addresses.
	 *
	 * Vector table at PA 0x00, entries are handler VAs.
	 * All handlers live on the vector page (VECTOR_VA + offset).
	 * Compute each VA = VECTOR_VA + 0x80 + (label - _real_miss_handler).
	 *
	 * Vector layout (from architecture spec):
	 *   0: bus fault  1: IRQ       2: TLB miss  3: TLB prot
	 *   4: privilege  5: SYSCALL   6: BREAK     7: illegal
	 *   8: alignment  9: EXT_IRQ  10: arith (DIV0)
	 */

	/* Helper: R3 = base = VECTOR_VA + 0x80 */
	lli	r3, #0xB080
	lui	r3, #0xFFFF
	/* R5 = link-time base of handler code */
	la	r5, _real_miss_handler

	/* Macro-like pattern: compute VA of each stub and store.
	 * VA = R3 + (stub_label - _real_miss_handler)
	 */

	/* Vector 2: TLB miss → _real_miss_handler (offset 0) */
	stw	r3, [r0 + #0x08]

	/* Vector 0: bus fault → _trap_entry_busfault */
	la	r1, _trap_entry_busfault
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x00]

	/* Vector 1: timer interrupt → _trap_entry_timer */
	la	r1, _trap_entry_timer
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x04]

	/* Vector 3: TLB prot → _trap_entry_tlbprot */
	la	r1, _trap_entry_tlbprot
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x0C]

	/* Vector 4: privilege → _trap_entry_priv */
	la	r1, _trap_entry_priv
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x10]

	/* Vector 5: SYSCALL → _trap_entry_syscall */
	la	r1, _trap_entry_syscall
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x14]

	/* Vector 6: BREAK → _trap_entry_break */
	la	r1, _trap_entry_break
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x18]

	/* Vector 7: illegal → _trap_entry_illegal */
	la	r1, _trap_entry_illegal
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x1C]

	/* Vector 8: alignment → _trap_entry_align */
	la	r1, _trap_entry_align
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x20]

	/* Vector 9: external device IRQ → _trap_entry_extirq */
	la	r1, _trap_entry_extirq
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x24]

	/* Vector 10: arithmetic fault (DIV0) → _trap_entry_arith */
	la	r1, _trap_entry_arith
	sub	r1, r5
	add	r1, r3
	stw	r1, [r0 + #0x28]

/* ═══════════════════════════════════════════════════════════════
 * Step 9: Pin TLB slots
 *
 * Slot 0: Vector page  — VECTOR_VA (0xFFFFB000) → PA 0x0
 * Slot 1: Kernel L1    — PT_KERN_L1_VA (0xFFFFE000) → PA of _boot_l1
 * Slot 4: User L1      — PT_USER_L1_VA (0xFFFFA000) → PA of _boot_l1
 *                        (default; pmap_activate reprograms per LWP)
 * ═══════════════════════════════════════════════════════════════ */

	/* Pin vector page (slot 0): VECTOR_VA (0xFFFFB000) → PA 0
	 * VPN = 0xFFFFB → TLB_VPN = 0x0FFFFB00
	 * PTE = PA 0 | V|C|R|W|X|G (0xBD — handler needs R+X, scratch needs R+W)
	 */
	lli	r1, #PTLB_VECTOR
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX

	lli	r1, #0xFB00
	lui	r1, #0x0FFF		/* R1 = 0x0FFFFB00 */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN

	lli	r1, #PTE_KERNEL		/* V|C|R|W|X|G */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

	/* Pin kernel L1 (slot 1): VA 0xFFFFE000 → PA of _boot_l1.
	 * This slot is permanent — the fast walker reads kernel-VA
	 * page-table entries from here regardless of which user pmap
	 * is active (SH-4-style split walker).
	 * VPN = 0xFFFFE → TLB_VPN = 0x0FFFFE00
	 * PTE = l1_pa | V|C|R|W|G (0x9D)
	 */
	lli	r1, #PTLB_KERN_L1
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX

	lli	r1, #0xFE00
	lui	r1, #0x0FFF		/* R1 = 0x0FFFFE00 */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN

	lli	r1, #0x9D		/* V|C|R|W|G */
	or	r1, r7			/* R7 = L1 PA (still in reg) */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

	/* Pin user L1 (slot 4) initially to the kernel L1 too.
	 * pmap_activate reprograms this slot per LWP; the default
	 * here means a stray user-VA access in kernel context
	 * walks kernel L1, finds no entry, and faults cleanly
	 * instead of recursing on an unmapped PT_USER_L1_VA.
	 * VPN = 0xFFFFA → TLB_VPN = 0x0FFFFA00.
	 */
	lli	r1, #PTLB_USER_L1
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX

	lli	r1, #0xFA00
	lui	r1, #0x0FFF		/* R1 = 0x0FFFFA00 */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN

	lli	r1, #0x9D		/* V|C|R|W|G */
	or	r1, r7			/* R7 = kernel L1 PA */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═══════════════════════════════════════════════════════════════
 * Step 10: Identity-map current page for MMU enable transition
 *
 * After WRSYS sets M=1, the next instruction fetch uses the
 * physical PC as a virtual address.  This TLB entry maps
 * VA=PA (identity) so the fetch succeeds.  The entry is
 * naturally evicted once we jump to kernel virtual addresses.
 * ═══════════════════════════════════════════════════════════════ */

.Lmmu_enable_ref:
	la	r1, .Lmmu_enable_ref
	add	r1, r6			/* R1 = physical address */
	shr	r1, #12			/* R1 = physical page number */

	/* TLB_INDEX: set = PPN[4:0], way 0 */
	mov	r2, r1
	and	r2, #0x1F
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_INDEX

	/* TLB_VPN: identity (VPN = PPN), ASID=0 */
	mov	r2, r1
	shl	r2, #8
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_VPN

	/* TLB_PTE: PPN << 12 | kernel flags */
	shl	r1, #12
	lli	r2, #PTE_KERNEL
	or	r1, r2
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═══════════════════════════════════════════════════════════════
 * Step 11: Enable MMU
 *
 * The real page-table-walking handler is active from this point.
 * Kernel image PTEs are in the page table.  L1 is pinned.
 * The first TLB miss after M=1 will be resolved by the handler.
 * ═══════════════════════════════════════════════════════════════ */

	lli	r1, #MMUCR_M
	wrsys	r1, #SYSDEV_MMU, #MMU_MMUCR

	/*
	 * MMU is now active.  Still at the physical PC, but it
	 * resolves via the identity-map TLB entry loaded above.
	 * Do NOT use the stack until SP is set to virtual.
	 */

	/*
	 * Enable both L1 caches.  PTE_KERNEL pages declare cacheability via
	 * PTE.C, but cache_active = CACHE_CTRL.ENABLE & PTE.C — without
	 * the master enable every fetch and load/store passes through to
	 * SDRAM at full CDC + 100 MHz CL2 latency.  Cache valid bits are
	 * cleared at reset and the caches were inert until now, so the
	 * earlier CACHE_INVAL_ALL writes (and these enables) take effect on
	 * a known-clean state — no extra invalidate needed here.
	 */
	lli	r1, #CACHE_CTRL_ENABLE
	wrsys	r1, #SYSDEV_L1_DCACHE, #CACHE_CTRL
	wrsys	r1, #SYSDEV_L1_ICACHE, #CACHE_CTRL

	/*
	 * Enable L2 cache if present.  The current bitstream always
	 * instantiates the L2, but a future variant could omit it; we
	 * probe SYSDEV_L2_CACHE.INFO and skip the enable when the device
	 * id is unmapped (RDSYS returns zero).  L2 is disabled at reset
	 * and passes traffic through until ENABLE is set.
	 */
	rdsys r1, #SYSDEV_L2_CACHE, #CACHE_INFO
	cmp   r1, #0
	beq   .Lno_l2
	lli   r1, #CACHE_CTRL_ENABLE
	wrsys r1, #SYSDEV_L2_CACHE, #CACHE_CTRL
.Lno_l2:

/* ═══════════════════════════════════════════════════════════════
 * Step 12: Jump to kernel virtual addresses
 * ═══════════════════════════════════════════════════════════════ */

	la	r13, .Lvirtual_entry
	ret				/* mov pc, lr → jump to virtual */

	/*
	 * ── Running at kernel virtual addresses from here ──
	 * BSS is already zeroed and bootinfo copied.
	 * TLB misses are resolved by the real page-table handler.
	 * Kernel image is mapped; MMIO is not (yet).
	 */
.Lvirtual_entry:
	/* Set up virtual boot stack (4 KB — only for early init) */
	la	sp, _boot_stack_top

	/*
	 * Pin curlwp in R12 (the kernel thread-pointer register) before any
	 * C code runs.  curlwp == &lwp0 until the first context switch.
	 */
	la	r12, lwp0

	/*
	 * Phase 1: early init on boot stack.
	 * penumbra_init() returns the new SP (lwp0 kernel stack top)
	 * in R1.  Must switch to it before calling main(), since
	 * the boot stack is too small for the full init call chain.
	 */
	bl	penumbra_init

	/* Switch to lwp0's kernel stack and continue boot */
	mov	sp, r1
	bl	penumbra_main
	break				/* NOTREACHED */


/* ═══════════════════════════════════════════════════════════════
 * Real TLB miss handler — page table walking (SH-4-style split).
 *
 * Walks a 2-level page table (L1 → L2) using pinned TLB slots:
 *   Slot 0: Vector page at VECTOR_VA  (handler code + scratch)
 *   Slot 1: Kernel L1   at PT_KERN_L1_VA (0xFFFFE000, permanent)
 *   Slot 2: L2 window   at PT_L2WIN_VA   (0xFFFFD000, remapped per miss)
 *   Slot 4: User L1     at PT_USER_L1_VA (0xFFFFA000, re-pinned by
 *                                          pmap_activate per LWP)
 *
 * The handler branches on the faulting VA's MSB: kernel VAs
 * (signed-negative) walk the kernel L1 via slot 1; user VAs walk
 * the current user L1 via slot 4.  No kernel-half mirroring in
 * user pmaps is needed — kernel and user L1s are completely
 * disjoint structures.
 *
 * This code is copied to vector page offset 0x80 by locore.S
 * during early boot (physical mode).  Active from MMU enable.
 * VA 0x0 is NOT mapped — the handler uses PC-relative addressing
 * for its scratch area, which lives after the handler code.
 *
 * All branches and scratch accesses are PC-relative — works
 * correctly regardless of where the code is mapped.
 * ═══════════════════════════════════════════════════════════════ */
	/*
	 * Page-align the trap-handler region so its kernel-text PTE
	 * can be marked R-only independently of the rest of .text.
	 * The working copies of these handlers live on the pinned
	 * vector page; this kernel-text copy must never execute or
	 * be written to — any such access is a bug (e.g. a stale
	 * `b trap_return` that resolves to the kernel-text VA).
	 * pmap_bootstrap() calls pmap_protect(..., VM_PROT_READ) on
	 * [_real_handlers_page_start, _real_handlers_page_end) so any
	 * execute or write fault here panics cleanly.  Reads still
	 * succeed so DDB / ksyms can inspect the page.
	 *
	 * The .balign 4096 also satisfies the original cache-line
	 * alignment requirement (handler prologue covers one L1 line)
	 * and the step-7 copy destination's multiple-of-16 constraint.
	 */
	.balign	4096
	.globl	_real_handlers_page_start
_real_handlers_page_start:
	.globl	_real_miss_handler
	.globl	_real_handlers_end
_real_miss_handler:
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
	blt	.Lrmh_kern_l1		/* VA < 0 (MSB set) → kernel L1 */
	lli	r3, #0xA000		/* user VA → PT_USER_L1_VA */
	b	.Lrmh_l1_hi
.Lrmh_kern_l1:
	lli	r3, #0xE000		/* kernel VA → PT_KERN_L1_VA */
.Lrmh_l1_hi:
	lui	r3, #0xFFFF		/* R3 = 0xFFFF_X000 selected above */
	add	r3, r2			/* R3 = &L1[index] */
	ldw	r3, [r3]		/* R3 = L1E (L2 PA | flags) */

	/* Check L1 entry valid bit */
	test	r3, #PTE_V
	beq	.Lrmh_fault		/* L1E invalid → page fault */

	/* ── Map L2 table into pinned window (slot 2) ──
	 * Extract L2 table PA from L1E (clear flag bits),
	 * then remap pinned slot 2 to point there.
	 *
	 * Window VA = 0xFFFFD000.  TLB entry:
	 *   TLB_INDEX = 0x42 (pinned bit | slot 2)
	 *   TLB_VPN  = (0xFFFFD >> 0) << 8 = 0x0FFFFD00
	 *   TLB_PTE  = L2_PA | V|C|R|G (0x8D)
	 */
	shr	r3, #PTE_PPN_SHIFT
	shl	r3, #PTE_PPN_SHIFT	/* R3 = L2 table PA (aligned) */

	lli	r2, #PTLB_L2WIN
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_INDEX

	lli	r2, #0xFD00
	lui	r2, #0x0FFF		/* R4 = 0x0FFFFD00 (TLB_VPN) */
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_VPN

	lli	r2, #0x8D		/* V|C|R|G = 0x01|0x04|0x08|0x80 */
	or	r2, r3			/* R4 = L2_PA | flags */
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
	beq	.Lrmh_fault		/* PTE invalid → page fault */

	/* ── Load PTE into main TLB ──────────────────
	 *
	 * Live state at this point:
	 *   R1 = faulting VA
	 *   R3 = PTE value (must be preserved for the final wrsys)
	 *   R2 = scratch (free)
	 */

	shr	r1, #12
	and	r1, #0x1F		/* R1 = set (bits 16:12 of VA) */

	/* Pick a way using the free-running cycle counter.
	 * XOR with the set bits before masking so adjacent-set
	 * misses spread across both ways even if cycle bit 5
	 * happens to be locked by a periodic workload. */
	rdsys	r2, #SYSDEV_CPU, #CPU_CYCLES
	xor	r2, r1
	and	r2, #0x20		/* isolate the way bit */
	or	r1, r2			/* R1 = {way, set} */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX

	rdsys	r1, #SYSDEV_MMU, #MMU_FAULT_ADDR
	shr	r1, #12
	shl	r1, #8
	test	r3, #PTE_G
	bne	.Lrmh_asid_done		/* G=1 → ASID=0, already in R1 */
	rdsys	r2, #SYSDEV_MMU, #MMU_MMUCR
	shr	r2, #MMUCR_ASID_SHIFT
	and	r2, #0xFF
	or	r1, r2
.Lrmh_asid_done:
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN
	wrsys	r3, #SYSDEV_MMU, #MMU_TLB_PTE

	/* Restore scratch registers and return */
	rdspr	r1, scr0
	rdspr	r2, scr1
	rdspr	r3, scr2
	eret

.Lrmh_fault:
	/*
	 * TLB miss handler failed to find a mapping.
	 * R1-R3 were parked in SCR0..2 on handler entry.
	 * Restore them and enter the common trap path with cause=2
	 * (EXC_TLB_MISS).
	 */
	rdspr	r1, scr0
	rdspr	r2, scr1
	rdspr	r3, scr2
	/* Fall through to _trap_entry_tlbmiss */

/* ── Per-vector trap entry stubs ─────────────────────────────
 *
 * Each stub saves R1 to scratch, loads the vector number into R1,
 * and branches to _trap_common.  The vector table points each
 * exception vector to the appropriate stub.
 *
 * These live on the vector page (pinned slot 0) so they are
 * always TLB-resident — no nested TLB miss on exception entry.
 *
 * Vector numbers (from architecture spec):
 *   0=bus fault, 1=IRQ, 2=TLB miss, 3=TLB prot,
 *   4=privilege, 5=SYSCALL, 6=BREAK, 7=illegal, 8=alignment
 */

_trap_entry_tlbmiss:	/* vector 2 — reached from .Lrmh_fault above */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #2
	b	_trap_common

_trap_entry_busfault:	/* vector 0 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #0
	b	_trap_common

_trap_entry_timer:	/* vector 1: timer interrupt */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #1
	b	_trap_common

_trap_entry_tlbprot:	/* vector 3 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #3
	b	_trap_common

_trap_entry_priv:	/* vector 4 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #4
	b	_trap_common

_trap_entry_syscall:	/* vector 5 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #5
	b	_trap_common

_trap_entry_break:	/* vector 6 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #6
	b	_trap_common

_trap_entry_illegal:	/* vector 7 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #7
	b	_trap_common

_trap_entry_align:	/* vector 8 */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #8
	b	_trap_common

_trap_entry_extirq:	/* vector 9: external device IRQ */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #9
	b	_trap_common

_trap_entry_arith:	/* vector 10: arithmetic fault (DIV0) */
	stw	r1, [pc + _trap_save_r1 - .]
	lli	r1, #10
	b	_trap_common

/* ── Common trap entry ───────────────────────────────────────
 *
 * On entry:
 *   R1 = vector number (cause)
 *   Original R1 saved in _trap_save_r1 (PC-relative scratch)
 *   R2-R13 = untouched from faulting context
 *   R14 = kernel SP (hardware switched on exception entry)
 *   EPC = faulting PC (SPR)
 *   ESR = faulting SR (SPR)
 *
 * Builds a struct trapframe on the kernel stack and calls
 * trap() in C.  On return, restores all registers and ERETs.
 */
_trap_common:
	/* Save R2-R4 to scratch (R1 already saved by stub) */
	stw	r2, [pc + _trap_save_r2 - .]
	stw	r3, [pc + _trap_save_r3 - .]
	stw	r4, [pc + _trap_save_r4 - .]

	/* R1 = cause number — stash in R4 (callee-clobberable) */
	mov	r4, r1

	/*
	 * Double-fault detection: if the faulting context was
	 * supervisor mode (ESR.S set) AND EPC is in the pinned
	 * page region (0xFFFFxxxx — vector page, scratch, L2
	 * window, L1), we faulted inside an exception handler.
	 * The kernel cannot recover — building a trapframe would
	 * just fault again.
	 *
	 * The check runs BEFORE the snapshot stores below, so on
	 * the double-fault path the _trap_save_hw_* slots still
	 * hold the previous trap through this code — for a fault
	 * in the trapframe build that is the victim whose entry
	 * faulted.  This fault's own state is still live in the
	 * SPRs and MMU fault sysregs; _dfault_print reads it from
	 * hardware.
	 *
	 * Both checks are needed: a user program dereferencing
	 * 0xFFFFxxxx triggers a legitimate TLB miss that should
	 * be dispatched normally (and will result in SIGSEGV).
	 *
	 * R1-R3 are freely clobberable (saved to scratch).
	 */
	rdspr	r1, esr
	li	r3, PSL_S
	and	r1, r3
	cmp	r1, r0
	beq	.Lnot_double_fault	/* user mode — not a double fault */
	rdspr	r1, epc
	lui	r3, #0xFFFF		/* R3 = 0xFFFF0000 */
	and	r1, r3			/* R1 = EPC & 0xFFFF0000 */
	cmp	r1, r3			/* EPC in 0xFFFFxxxx range? */
	bne	.Lnot_double_fault
	/*
	 * Double fault confirmed.  On gen2 BREAK is a trapping
	 * exception, not a halt: a BREAK here re-enters _trap_common,
	 * re-trips this same test (its own EPC is supervisor and on the
	 * vector page), and storms forever.  Branch instead to
	 * _dfault_print, which prints this fault's live hardware state
	 * and the victim's still-intact snapshot out the hardwired
	 * console UART and then spins.  No faultable memory is touched,
	 * so the evidence survives.
	 */
	b	_dfault_print
.Lnot_double_fault:

	/*
	 * Snapshot volatile hardware state into pinned scratch.
	 *
	 * Must happen BEFORE any faultable memory access.  The
	 * trapframe lives on the kernel stack, which can TLB-miss.
	 * A resolved miss clobbers ESR, EPC, FAULT_ADDR, and
	 * FAULT_STATUS via the handler's ERET.  All stores here
	 * are PC-relative into the pinned vector page — no TLB
	 * miss possible.
	 *
	 * R1, R2, R3 are free (originals saved in scratch above).
	 * R4 = cause number (must preserve).
	 */
	rdspr	r1, esr
	stw	r1, [pc + _trap_save_hw_esr - .]
	rdspr	r2, epc
	stw	r2, [pc + _trap_save_hw_epc - .]
	rdsys	r3, #SYSDEV_MMU, #MMU_FAULT_ADDR
	stw	r3, [pc + _trap_save_hw_badvaddr - .]
	rdsys	r3, #SYSDEV_MMU, #MMU_FAULT_STATUS
	stw	r3, [pc + _trap_save_hw_fstat - .]
	stw	r4, [pc + _trap_save_hw_cause - .]

	/* Allocate trapframe on kernel stack */
	sub	r14, #TF_SIZE

	/* Save R0 (always 0, but keep trapframe consistent) */
	stw	r0, [r14 + TF_R0]

	/* Recover original R1 from scratch and save it */
	ldw	r1, [pc + _trap_save_r1 - .]
	stw	r1, [r14 + TF_R0 + 4]

	/* Save R2-R4 from scratch */
	ldw	r1, [pc + _trap_save_r2 - .]
	stw	r1, [r14 + TF_R0 + 8]
	ldw	r1, [pc + _trap_save_r3 - .]
	stw	r1, [r14 + TF_R0 + 12]
	ldw	r1, [pc + _trap_save_r4 - .]
	stw	r1, [r14 + TF_R0 + 16]

	/* Save R5-R13 directly */
	stw	r5,  [r14 + TF_R0 + 20]
	stw	r6,  [r14 + TF_R0 + 24]
	stw	r7,  [r14 + TF_R0 + 28]
	stw	r8,  [r14 + TF_R0 + 32]
	stw	r9,  [r14 + TF_R0 + 36]
	stw	r10, [r14 + TF_R0 + 40]
	stw	r11, [r14 + TF_R0 + 44]
	stw	r12, [r14 + TF_R0 + 48]
	stw	r13, [r14 + TF_R0 + 52]

	/*
	 * Save pre-exception SP.  If we came from user mode, the
	 * hardware swapped SP↔USP on entry, so the user SP is now
	 * in the USP SPR.  If from kernel mode, R14 was the kernel
	 * SP and we subtracted TF_SIZE, so the original is R14+TF_SIZE.
	 *
	 * Read ESR from pinned scratch (not from SPR — a TLB miss
	 * during the register saves above may have clobbered it).
	 */
	ldw	r1, [pc + _trap_save_hw_esr - .]
	li	r2, PSL_S
	and	r1, r2			/* Z=1 if from user mode */
	bne	.Lsave_kern_sp
	rdspr	r1, usp			/* user mode: SP was banked */
	b	.Lsave_sp_done
.Lsave_kern_sp:
	mov	r1, r14			/* kernel mode: undo our SUB */
	add	r1, #TF_SIZE
.Lsave_sp_done:
	stw	r1, [r14 + TF_R0 + 56]		/* tf_regs[14] = original SP */

	/* Save EPC as tf_regs[15] (PC at time of exception) */
	ldw	r1, [pc + _trap_save_hw_epc - .]
	stw	r1, [r14 + TF_R0 + 60]		/* tf_regs[15] */

	/* Save ESR, EPC, cause, badvaddr, fstat — all from scratch */
	ldw	r1, [pc + _trap_save_hw_esr - .]
	stw	r1, [r14 + TF_SR]
	ldw	r1, [pc + _trap_save_hw_epc - .]
	stw	r1, [r14 + TF_EPC]
	stw	r4, [r14 + TF_CAUSE]
	ldw	r1, [pc + _trap_save_hw_badvaddr - .]
	stw	r1, [r14 + TF_BADVADDR]
	ldw	r1, [pc + _trap_save_hw_fstat - .]
	stw	r1, [r14 + TF_FAULT_STATUS]

	/*
	 * Establish curlwp in R12 for the kernel C code about to run.  On
	 * entry from user mode R12 held the user's TLS pointer (already saved
	 * to the trapframe above); on entry from kernel mode it already held
	 * curlwp.  Reload unconditionally from the canonical copy — a register
	 * load is cheaper than branching on the entry mode and is correct
	 * either way.  (CI_CURLWP is offset 0: ci_curlwp is first in cpu_info.)
	 */
	la	r12, cpu_info_store
	ldw	r12, [r12 + CI_CURLWP]

	/* Call trap(tf) — R1 = pointer to trapframe */
	mov	r1, r14
	la	r11, trap
	jalr	r11

	/* ── Return from trap ────────────────────────────────
	 *
	 * Restore all registers from trapframe and ERET.
	 * R14 must point to the trapframe.
	 * Also used by lwp_trampoline to return to userland
	 * after exec/fork.
	 *
	 * CRITICAL: A TLB miss between wrspr ESR/EPC and eret
	 * would overwrite ESR/EPC with the current supervisor
	 * context.  To avoid this, all trapframe loads happen
	 * first, with EPC/ESR/R1/R2 stashed in the pinned
	 * vector page scratch area.  The final sequence loads
	 * from pinned scratch (no TLB miss) and erets.
	 *
	 * TODO: check for AST/signal delivery before returning
	 * to userspace.
	 */
	.globl	trap_return
trap_return:
	/*
	 * Interrupts MUST already be masked on entry here (see the DI in
	 * trap() for the C-handler door and in lwp_trampoline for the
	 * fork/exec door).  The masking cannot be done here: this code
	 * runs on the pinned vector page, so an IRQ taken on a DI placed
	 * here would have EPC in the 0xFFFFxxxx range and trip the
	 * double-fault guard.  Each kernel-text caller masks before
	 * transferring control into this epilogue.
	 */

	/* If returning to user mode, set USP from trapframe */
	ldw	r1, [r14 + TF_SR]
	li	r2, PSL_S
	and	r1, r2			/* Z=1 if returning to user mode */
	bne	.Lrestore_regs		/* returning to kernel — skip USP */
	ldw	r1, [r14 + TF_R0 + 56]	/* tf_regs[14] = user SP */
	wrspr	usp, r1

.Lrestore_regs:
	/*
	 * Stash EPC, ESR, R1, R2 in the pinned vector page scratch
	 * area.  All further loads are from the trapframe (may TLB
	 * miss — safe because ESR/EPC haven't been written yet).
	 * After all trapframe loads, we read back from the pinned
	 * scratch (no TLB miss) and write SPRs immediately before eret.
	 */
	ldw	r1, [r14 + TF_EPC]
	stw	r1, [pc + _trap_save_r3 - .]
	ldw	r1, [r14 + TF_SR]
	stw	r1, [pc + _trap_save_r4 - .]
	ldw	r1, [r14 + TF_R0 + 4]		/* user R1 */
	stw	r1, [pc + _trap_save_r1 - .]
	ldw	r1, [r14 + TF_R0 + 8]		/* user R2 */
	stw	r1, [pc + _trap_save_r2 - .]

	/* Restore R3-R13 from trapframe (may TLB miss — safe) */
	ldw	r3,  [r14 + TF_R0 + 12]
	ldw	r4,  [r14 + TF_R0 + 16]
	ldw	r5,  [r14 + TF_R0 + 20]
	ldw	r6,  [r14 + TF_R0 + 24]
	ldw	r7,  [r14 + TF_R0 + 28]
	ldw	r8,  [r14 + TF_R0 + 32]
	ldw	r9,  [r14 + TF_R0 + 36]
	ldw	r10, [r14 + TF_R0 + 40]
	ldw	r11, [r14 + TF_R0 + 44]
	ldw	r12, [r14 + TF_R0 + 48]
	ldw	r13, [r14 + TF_R0 + 52]

	/* Deallocate trapframe (restore kernel SP) */
	add	r14, #TF_SIZE

	/*
	 * Final sequence: all loads from PINNED vector page scratch.
	 * No TLB miss possible — ESR/EPC writes are safe.
	 */
	ldw	r1, [pc + _trap_save_r3 - .]	/* EPC */
	wrspr	epc, r1
	ldw	r1, [pc + _trap_save_r4 - .]	/* ESR */
	wrspr	esr, r1
	ldw	r1, [pc + _trap_save_r1 - .]	/* user R1 */
	ldw	r2, [pc + _trap_save_r2 - .]	/* user R2 */

	/* Return from exception — restores PC from EPC, SR from ESR.
	 * If returning to user mode, hardware swaps SP ↔ USP. */
	eret

/* ── Trap handler scratch data ──────────────────────────────
 * Separate from the TLB miss handler scratch.
 * Accessed via PC-relative addressing.
 *
 * The _trap_save_hw_* slots capture volatile hardware state
 * (ESR, EPC, FAULT_ADDR, FAULT_STATUS) before any faultable
 * memory access.  A TLB miss during trapframe allocation would
 * otherwise clobber these registers when the miss handler
 * succeeds and ERETs.
 */
	.balign	4
_trap_save_r1:
	.long	0
_trap_save_r2:
	.long	0
_trap_save_r3:
	.long	0
_trap_save_r4:
	.long	0
_trap_save_hw_esr:
	.long	0
_trap_save_hw_epc:
	.long	0
_trap_save_hw_badvaddr:
	.long	0
_trap_save_hw_fstat:
	.long	0
_trap_save_hw_cause:
	.long	0

/* ═══════════════════════════════════════════════════════════════
 * _dfault_print — last-gasp fault dump for the double-fault path
 *
 * Reached by BRANCH (never call) from _trap_common when a fault is
 * taken inside an exception handler.  Lives on the pinned vector page,
 * so every branch and scratch reference is PC-relative and no
 * faultable memory is touched — it cannot itself fault.
 *
 * Prints two fault records out the console UART, then spins.  The
 * unprefixed lines describe THIS fault — the one that tripped the
 * guard — read live from the SPRs and MMU fault sysregs (nothing has
 * faulted since, so they still hold its state), plus the supervisor
 * SP.  The P-prefixed lines are the _trap_save_hw_* snapshot: the
 * previous trap through _trap_common, which the guard deliberately
 * did not overwrite.  For a fault in the trapframe build that is the
 * victim whose handler entry faulted; for a fault inside the TLB
 * miss fastpath it is merely the last full trap taken.
 *
 * The UART is the architecturally fixed console at 0xFF000000 — the
 * only hardwired I/O address (doc/system/bus.md).  We reach it
 * without trusting the kernel's discovered uart_base (potentially the
 * corrupt thing) by re-pinning the scratch TLB slot to it.  All
 * registers are clobbered; this never returns.
 * ═══════════════════════════════════════════════════════════════ */
	.balign	4
	.globl	_dfault_print
_dfault_print:
	/* Re-pin scratch slot → UART physical page, uncached.  WRSYS
	 * only; no memory access, so it cannot fault. */
	li	r1, #PTLB_SCRATCH
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	li	r1, #0x0FFFFC00			/* VPN of SCRATCH_VA (0xFFFFC000), ASID 0 */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_VPN
	li	r1, #0xFF000000			/* hardwired console UART page (bus.md) */
	lli	r3, #PTE_KERNEL_NC		/* uncached kernel R/W */
	or	r1, r3
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE	/* commit pin */

	/* Banner: "\r\nDBLFLT\r\n" */
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc
	lli	r2, #'D';	bl _df_putc
	lli	r2, #'B';	bl _df_putc
	lli	r2, #'L';	bl _df_putc
	lli	r2, #'F';	bl _df_putc
	lli	r2, #'L';	bl _df_putc
	lli	r2, #'T';	bl _df_putc
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* VEC=<hex>  (this fault's vector — live in R4 from the stub;
	 * the banner putc calls clobber only R1/R3) */
	lli	r2, #'V';	bl _df_putc
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'C';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* ESR=<hex>  (this fault's ESR, live from the SPR) */
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'S';	bl _df_putc
	lli	r2, #'R';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	rdspr	r4, esr
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* EPC=<hex>  (this fault's PC, live from the SPR) */
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'C';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	rdspr	r4, epc
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* VA=<hex>   (this fault's virtual address, live) */
	lli	r2, #'V';	bl _df_putc
	lli	r2, #'A';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	rdsys	r4, #SYSDEV_MMU, #MMU_FAULT_ADDR
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* FST=<hex>  (this fault's status, live) */
	lli	r2, #'F';	bl _df_putc
	lli	r2, #'S';	bl _df_putc
	lli	r2, #'T';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	rdsys	r4, #SYSDEV_MMU, #MMU_FAULT_STATUS
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* SP=<hex>   (supervisor SP — untouched by the print helpers) */
	lli	r2, #'S';	bl _df_putc
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	mov	r4, r14
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* P-block: the previous trap's snapshot (see header comment). */

	/* PVEC=<hex> */
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'V';	bl _df_putc
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'C';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	ldw	r4, [pc + _trap_save_hw_cause - .]
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* PESR=<hex> */
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'S';	bl _df_putc
	lli	r2, #'R';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	ldw	r4, [pc + _trap_save_hw_esr - .]
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* PEPC=<hex> */
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'E';	bl _df_putc
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'C';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	ldw	r4, [pc + _trap_save_hw_epc - .]
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* PVA=<hex> */
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'V';	bl _df_putc
	lli	r2, #'A';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	ldw	r4, [pc + _trap_save_hw_badvaddr - .]
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* PFST=<hex> */
	lli	r2, #'P';	bl _df_putc
	lli	r2, #'F';	bl _df_putc
	lli	r2, #'S';	bl _df_putc
	lli	r2, #'T';	bl _df_putc
	lli	r2, #'=';	bl _df_putc
	ldw	r4, [pc + _trap_save_hw_fstat - .]
	bl	_df_puthex
	lli	r2, #13;	bl _df_putc
	lli	r2, #10;	bl _df_putc

	/* Quiet halt.  IRQs masked, no faultable memory touched, so the
	 * saved state stays intact and the spin PC is stable (readable on
	 * the debug LEDs / in the ISS). */
.Ldf_spin:
	b	.Ldf_spin

/* _df_putc — poll-write one byte (R2) to the console UART via the
 * re-pinned scratch window.  Clobbers R1, R3.  Returns via R13. */
_df_putc:
	li	r3, #SCRATCH_VA			/* UART aliased here */
.Ldf_pc_wait:
	ldw	r1, [r3 + #0x14]		/* UART LSR */
	test	r1, #0x20			/* TX holding empty? */
	beq	.Ldf_pc_wait
	stw	r2, [r3 + #0x00]		/* UART THR */
	ret

/* _df_puthex — print R4 as 8 hex digits, MSB first.  Clobbers
 * R1,R2,R3,R6,R7; saves the return LR in R10 across the putc calls. */
_df_puthex:
	mov	r10, r13			/* save LR (putc clobbers R13) */
	mov	r6, r4				/* working copy */
	lli	r7, #8				/* nibble count */
.Ldf_hx_loop:
	mov	r1, r6
	shr	r1, #28				/* top nibble → R1[3:0] */
	and	r1, #0xF
	mov	r2, r1
	add	r2, #0x30			/* '0' + nibble */
	cmp	r1, #10
	blt	.Ldf_hx_emit
	add	r2, #7				/* bump '0'+10.. to 'A'..'F' */
.Ldf_hx_emit:
	bl	_df_putc
	shl	r6, #4				/* next nibble up */
	sub	r7, #1
	cmp	r7, #0
	bne	.Ldf_hx_loop
	mov	r13, r10
	ret

	.balign	4
_real_handlers_end:

	/* End of the R-only handler page — see _real_handlers_page_start. */
	.balign	4096
	.globl	_real_handlers_page_end
_real_handlers_page_end:


/* ═══════════════════════════════════════════════════════════════
 * TLB invalidation routines — called from C (pmap.c).
 * ═══════════════════════════════════════════════════════════════ */

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


/*
 * pmap_scratch_map(paddr_t pa, pt_entry_t flags)
 *
 * Map a physical page at SCRATCH_VA (0xFFFFC000) via pinned slot 3.
 * Used by C code to access pages that have no kernel VA yet.
 *
 * R1 = physical address (page-aligned)
 * R2 = PTE flags (e.g., PTE_KERNEL or PTE_KERNEL_NC)
 */
	.globl	pmap_scratch_map
	.type	pmap_scratch_map, @function
pmap_scratch_map:
	/* TLB_INDEX = pinned slot 3 */
	lli	r3, #PTLB_SCRATCH
	wrsys	r3, #SYSDEV_MMU, #MMU_TLB_INDEX

	/* TLB_VPN for SCRATCH_VA = 0xFFFFC000
	 * VPN = 0xFFFFC → (0xFFFFC << 8) = 0x0FFFFC00
	 */
	lli	r3, #0xFC00
	lui	r3, #0x0FFF		/* R3 = 0x0FFFFC00 */
	wrsys	r3, #SYSDEV_MMU, #MMU_TLB_VPN

	/* TLB_PTE = PA | flags */
	or	r1, r2			/* R1 = PA | flags */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

	ret


/*
 * cpu_switchto(oldlwp, newlwp, returning)
 *
 * Switch context from oldlwp (R1) to newlwp (R2).
 * Returns oldlwp in R1 (so the resumed LWP knows who it replaced).
 *
 * If oldlwp == NULL (first switch after boot), skip saving.
 *
 * Context is saved/restored via pcb_context (label_t):
 *   [0]=R5 [4]=R6 [8]=R7 [12]=R8 [16]=R9 [20]=R10
 *   [24]=R12(TP) [28]=R13(LR) [32]=R14(SP)
 */
	.globl	cpu_switchto
	.type	cpu_switchto, @function
cpu_switchto:
	/* Save old context if oldlwp != NULL */
	cmp	r1, r0
	beq	.Lswitch_restore

	/*
	 * R1 = oldlwp; save into oldlwp->l_addr->pcb_context.
	 * pcb_context is at offset 0 in struct pcb (first field),
	 * and pcb is at l_addr (= L_PCB offset in struct lwp).
	 * label_t.val[]: [0]=R5 [1]=R6 ... [7]=R13(LR) [8]=R14(SP)
	 */
	ldw	r11, [r1 + L_PCB]		/* R11 = old PCB = &pcb_context */

	stw	r5,  [r11 + 0]
	stw	r6,  [r11 + 4]
	stw	r7,  [r11 + 8]
	stw	r8,  [r11 + 12]
	stw	r9,  [r11 + 16]
	stw	r10, [r11 + 20]
	stw	r12, [r11 + 24]
	stw	r13, [r11 + 28]
	stw	r14, [r11 + 32]

.Lswitch_restore:
	/* R2 = newlwp; stash in callee-saved regs for the restore */
	mov	r5, r1			/* R5 = oldlwp (return value) */
	mov	r6, r2			/* R6 = newlwp */

	/* Update curlwp: cpu_info_store.ci_curlwp = newlwp */
	la	r11, cpu_info_store
	stw	r6, [r11 + CI_CURLWP]

	/* Restore new context from newlwp->l_addr->pcb_context */
	ldw	r11, [r6 + L_PCB]		/* R11 = new PCB = &pcb_context */

	ldw	r6,  [r11 + 4]
	ldw	r7,  [r11 + 8]
	ldw	r8,  [r11 + 12]
	ldw	r9,  [r11 + 16]
	ldw	r10, [r11 + 20]
	ldw	r12, [r11 + 24]
	ldw	r13, [r11 + 28]
	ldw	r14, [r11 + 32]

	/*
	 * Sanity check: SP must be in kernel VA space (bit 31 set).
	 * If not, the pcb_context is corrupt — BREAK to halt with
	 * diagnostics instead of crashing in an unrecoverable way.
	 * Uses R1 as scratch (will be overwritten below anyway).
	 */
	li	r1, 0x80000000		/* test bit 31 of new SP */
	and	r1, r14
	cmp	r1, r0
	bne	.Lsp_restore_ok
	break				/* bad pcb_context — halt for debug */
.Lsp_restore_ok:

	/* Return oldlwp (in R5, need it in R1) */
	mov	r1, r5
	ldw	r5,  [r11 + 0]			/* now safe to restore R5 */
	ret

/*
 * lwp_trampoline — entry point for newly-forked LWPs.
 *
 * cpu_switchto restores pcb_context which has LR = lwp_trampoline.
 * At entry (from the pcb_context set up by cpu_lwp_fork):
 *   R1 = prev_lwp (cpu_switchto return value)
 *   R5 = func     (the LWP's startup function)
 *   R6 = arg      (argument to func)
 *   R7 = newlwp   (the LWP being started)
 *
 * Must call lwp_startup(prev, newlwp) first to finish the context
 * switch (unlock prev, clear LP_RUNNING, reset SPL).
 * Then calls func(arg), which either:
 *   - For kernel threads: runs the thread body (never returns)
 *   - For fork children: calls md_child_return → returns to user
 */
	.globl	lwp_trampoline
	.type	lwp_trampoline, @function
lwp_trampoline:
	/* R1 = prev_lwp (already in place as first arg) */
	mov	r2, r7			/* R2 = newlwp (second arg) */
	bl	lwp_startup

	mov	r1, r6			/* arg → R1 (first argument) */
	jalr	r5			/* call func(arg) */

	/*
	 * func returned — this is an exec'd or fork'd user process.
	 * Load the trapframe pointer from curlwp->l_md.md_utf and
	 * jump to the vector-page copy of trap_return.
	 *
	 * NOTE: a plain `b trap_return` would resolve to the kernel-
	 * text address, which is mapped R-only (no X) — the working
	 * copies of the handlers live on the pinned vector page so
	 * trap_return's PC-relative scratch is always TLB-resident.
	 * Indirect jump to the vector-page VA, computed at link time:
	 *   VECTOR_VA + 0x80 + (trap_return - _real_miss_handler)
	 * Both offsets are link-time constants (same translation unit).
	 */
	la	r11, cpu_info_store
	ldw	r11, [r11 + CI_CURLWP]	/* curlwp */
	ldw	r14, [r11 + L_MD_UTF]	/* md_utf → trapframe pointer */
	ldw	r11, [pc + .Lvp_trap_return - .]
	/*
	 * Mask interrupts before crossing into the vector-page trap
	 * epilogue (it writes the single EPC/ESR SPRs and must run
	 * uninterrupted).  Like trap()'s DI on the C-handler return path,
	 * this MUST happen here in kernel text: an IRQ taken on this di
	 * has EPC in kernel text and nests recoverably, whereas a di
	 * inside the epilogue would take the IRQ with EPC on the vector
	 * page and trip the double-fault guard.  The guard stays a cheap,
	 * strict, no-special-case assertion precisely because every door
	 * into the epilogue masks first — and it is the fail-loud backstop
	 * for any future door that forgets to.
	 */
	di
	jmp	r11

	.balign	4
.Lvp_trap_return:
	.long	VECTOR_VA + 0x80 + (trap_return - _real_miss_handler)

/*
 * setjmp / longjmp for kernel use (label_t).
 * Save/restore callee-saved registers.
 */
	.globl	setjmp
	.type	setjmp, @function
setjmp:
	/* R1 = pointer to label_t
	 * Save callee-saved regs + LR + SP.
	 * No need to save PC — LR already holds our caller's return address.
	 */
	stw	r5,  [r1 + 0]
	stw	r6,  [r1 + 4]
	stw	r7,  [r1 + 8]
	stw	r8,  [r1 + 12]
	stw	r9,  [r1 + 16]
	stw	r10, [r1 + 20]
	stw	r12, [r1 + 24]
	stw	r13, [r1 + 28]
	stw	r14, [r1 + 32]
	mov	r1, r0		/* return 0 */
	ret

	.globl	longjmp
	.type	longjmp, @function
longjmp:
	/* R1 = pointer to label_t
	 * Restore callee-saved regs + LR + SP, return 1.
	 * After restoring R13 (LR), ret jumps back to setjmp's caller.
	 */
	ldw	r5,  [r1 + 0]
	ldw	r6,  [r1 + 4]
	ldw	r7,  [r1 + 8]
	ldw	r8,  [r1 + 12]
	ldw	r9,  [r1 + 16]
	ldw	r10, [r1 + 20]
	ldw	r12, [r1 + 24]
	ldw	r13, [r1 + 28]
	ldw	r14, [r1 + 32]
	lli	r1, 1		/* return 1 */
	ret

/*
 * phys_bias: physical - virtual address offset (PA = VA + phys_bias).
 * Set by _start before BSS zero (stored to physical address).
 * Used during bootstrap for PA↔VA conversions.
 */
	.data
	.align	4
	.globl	phys_bias
phys_bias:
	.long	0

/*
 * Early boot stack — 4 KB, in BSS.
 */
	.bss
	.align	4
	.globl	_boot_stack
_boot_stack:
	.space	4096
	.globl	_boot_stack_top
_boot_stack_top:

/*
 * Kernel page table — pre-allocated in BSS (zeroed by step 4).
 * L1: 1 page (4 KB) = 1024 L1 entries.
 * L2: BOOT_NL2 pages (12 KB) = 3 × 1024 PTE entries = 12 MB coverage.
 * These are adopted by pmap_bootstrap() as the permanent kernel
 * page table.  The L1 is pinned in TLB slot 1 from early boot.
 */
	.balign	4096
	.globl	_boot_l1
_boot_l1:
	.space	4096
	.globl	_boot_l2
_boot_l2:
	.space	(BOOT_NL2 * 4096)

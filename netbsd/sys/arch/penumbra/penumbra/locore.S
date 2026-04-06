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
 *   SR.S = 1 (supervisor), SR.M = 0 (MMU off), SR.I = 0 (irqs off)
 *
 * The kernel is linked at KERNEL_TEXT_BASE (0x8001_0000).
 * locore.S must: compute virt-to-phys bias, set up initial TLB
 * entries, enable the MMU, and jump to virtual addresses before
 * any C code can run.
 *
 * Exception vectors live at physical 0x0000_0000:
 *   0x00: reset        0x04: IRQ
 *   0x08: TLB miss     0x0C: TLB protection
 *   0x10: bus fault    0x14: BREAK
 *   0x18: SYSCALL      0x1C: illegal / privilege
 */

#include "assym.h"
#include <machine/sysreg.h>
#include <machine/pmap.h>

/*
 * Bootstrap constants (assembly-safe, must match pmap.h).
 * Used by the bootstrap TLB miss handler until pmap takes over.
 */
#define BPTE_KERNEL	0xBD		/* V|C|R|W|X|G */
#define BPTE_KERNEL_NC	0x99		/* V|R|W|G (uncached, for MMIO) */
#define KERN_TEXT_VA	0x80010000	/* KERNEL_TEXT_BASE */
#define MMIO_BASE_VA	0xFF000000	/* Device MMIO region start */

	.text
	.globl	_start
	.type	_start, @function

/*
 * Kernel entry point.
 * Called by bootloader with:
 *   R1 = bootinfo physical address
 *   R2 = kernel physical entry point
 *   SR.S = 1 (supervisor), SR.M = 0 (MMU off), SR.I = 0 (irqs off)
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
	wrsys	r0, #SYSDEV_ICACHE, #CACHE_INVAL

/* ═══════════════════════════════════════════════════════════════
 * Step 4: Copy bootstrap TLB miss handler to vector page
 *
 * The handler code lives in the kernel image (.text).  We copy
 * it to PA 0x80 in the vector page, where it runs from pinned
 * TLB slot 0 after MMU enable.
 *
 * Vector page layout (physical page 0):
 *   0x00-0x1F   Exception vectors (8 × 4-byte addresses)
 *   0x40-0x4F   Scratch register save area (R1-R4)
 *   0x50-0x5B   Boot params (phys_bias, kernel_end, way_ctr)
 *   0x5C        Saved bootinfo physical pointer
 *   0x80+       TLB miss handler code
 * ═══════════════════════════════════════════════════════════════ */

	la	r1, _boot_miss_handler
	add	r1, r6			/* physical source address */
	lli	r2, #0x80		/* destination in vector page */
	la	r3, _boot_handlers_end
	add	r3, r6			/* physical end of handler code */
.Lcopy_handler:
	ldw	r4, [r1]
	stw	r4, [r2]
	add	r1, #4
	add	r2, #4
	cmp	r1, r3
	blo	.Lcopy_handler

	/* I-cache invalidate after writing code to vector page */
	wrsys	r0, #SYSDEV_ICACHE, #CACHE_INVAL

/* ═══════════════════════════════════════════════════════════════
 * Step 5: Install exception vectors and boot parameters
 * ═══════════════════════════════════════════════════════════════ */

	/* TLB miss vector → handler at PA 0x80 */
	lli	r1, #0x80
	stw	r1, [r0 + #0x08]

	/* Compute PA of panic handler in vector page */
	la	r1, _boot_prot_panic
	la	r2, _boot_miss_handler
	sub	r1, r2			/* offset from handler start */
	add	r1, #0x80		/* PA in vector page */

	/* All other vectors → panic for now */
	stw	r1, [r0 + #0x0C]	/* TLB protection fault */
	stw	r1, [r0 + #0x10]	/* bus fault */
	stw	r1, [r0 + #0x14]	/* BREAK */
	stw	r1, [r0 + #0x18]	/* SYSCALL */
	stw	r1, [r0 + #0x1C]	/* illegal / privilege */

	/* Boot parameters for handler scratch area */
	stw	r6, [r0 + #0x50]	/* phys_bias */
	la	r1, _end
	add	r1, #0xFFF		/* round up to page boundary */
	shr	r1, #12
	shl	r1, #12
	stw	r1, [r0 + #0x54]	/* kernel_end (VA, page-aligned) */
	stw	r0, [r0 + #0x58]	/* way_counter = 0 */
	stw	r5, [r0 + #0x5C]	/* bootinfo physical pointer */

/* ═══════════════════════════════════════════════════════════════
 * Step 6: Pin the vector/handler page (pinned TLB slot 0)
 *
 * Maps VA 0x0000_0000 → PA 0x0000_0000 (identity).
 * Covers: exception vectors, scratch save area, handler code.
 * ═══════════════════════════════════════════════════════════════ */

	lli	r1, #TLB_INDEX_PINNED	/* 0x40 = pinned slot 0 */
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_VPN	/* VPN=0, ASID=0 */
	lli	r1, #BPTE_KERNEL
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═══════════════════════════════════════════════════════════════
 * Step 7: Identity-map current page for MMU enable transition
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
	lli	r2, #BPTE_KERNEL
	or	r1, r2
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═══════════════════════════════════════════════════════════════
 * Step 8: Enable MMU
 * ═══════════════════════════════════════════════════════════════ */

	lli	r1, #MMUCR_M
	wrsys	r1, #SYSDEV_MMU, #MMU_MMUCR

	/*
	 * MMU is now active.  Still at the physical PC, but it
	 * resolves via the identity-map TLB entry loaded above.
	 * Do NOT use the stack until SP is set to virtual.
	 */

/* ═══════════════════════════════════════════════════════════════
 * Step 9: Jump to kernel virtual addresses
 * ═══════════════════════════════════════════════════════════════ */

	la	r13, .Lvirtual_entry
	ret				/* mov pc, lr → jump to virtual */

	/*
	 * ── Running at kernel virtual addresses from here ──
	 * TLB misses are resolved by the bootstrap handler.
	 */
.Lvirtual_entry:
	/* Set up virtual kernel stack */
	la	sp, _boot_stack_top

	/* Zero BSS */
	la	r1, __bss_start
	la	r2, _end
.Lzero_bss:
	cmp	r1, r2
	bhs	.Lbss_done
	stw	r0, [r1]
	add	r1, #4
	b	.Lzero_bss
.Lbss_done:

	/* Save bootinfo pointer to global (BSS, now zeroed) */
	ldw	r1, [r0 + #0x5C]	/* from vector page scratch area */
	la	r2, penumbra_bootinfo
	stw	r1, [r2]

	/* Call penumbra_init() — does not return */
	bl	penumbra_init
	break				/* NOTREACHED */

/* ═══════════════════════════════════════════════════════════════
 * Bootstrap TLB miss handler
 *
 * Copied to the vector page at PA 0x80 during early boot.
 * Resolves TLB misses using algorithmic VA→PA mapping:
 *   - Kernel pages: PA = VA + phys_bias (linear offset)
 *   - MMIO pages:   PA = VA (identity, uncached)
 *   - Anything else: panic (unmapped address)
 *
 * Runs until pmap_bootstrap() installs the real handler.
 * All branches are PC-relative, so the code works after copy.
 * Scratch register save at VA 0x40, boot params at VA 0x50.
 * ═══════════════════════════════════════════════════════════════ */
	.balign	4
_boot_miss_handler:
	/* Save scratch registers to vector page */
	stw	r1, [r0 + #0x40]
	stw	r2, [r0 + #0x44]
	stw	r3, [r0 + #0x48]
	stw	r4, [r0 + #0x4C]

	/* Read faulting virtual address */
	rdsys	r1, #SYSDEV_MMU, #MMU_FAULT_ADDR

	/* ── Kernel VA range: KERN_TEXT_VA ≤ VA < kernel_end ── */
	li	r2, #KERN_TEXT_VA
	cmp	r1, r2
	blo	.Lbmh_check_mmio
	ldw	r2, [r0 + #0x54]	/* kernel_end (VA) */
	cmp	r1, r2
	bhs	.Lbmh_check_mmio

	/* ── Kernel page: PA = VA + phys_bias ────────────────
	 * R1 = faulting VA (in kernel range)
	 * R2 = phys_bias (loaded below)
	 * Need to produce R3 = PTE value, then branch to
	 * .Lbmh_load_tlb.
	 */
	ldw	r2, [r0 + #0x50]	/* R2 = phys_bias */

	mov	r3, r1			/* R3 = VA */
	add	r3, r2			/* R3 = VA + phys_bias = PA */
	li	r2, #PTE_PPN_MASK
	and	r3, r2			/* clear page offset */
	lli	r2, #BPTE_KERNEL
	or	r3, r2			/* R3 = PTE */
	b	.Lbmh_load_tlb

.Lbmh_check_mmio:
	/* ── Device MMIO: VA ≥ 0xFF000000, identity mapped ──
	 * TODO: MMIO base should come from bootinfo device list
	 * rather than being hardcoded to 0xFF000000.
	 */
	li	r2, #MMIO_BASE_VA
	cmp	r1, r2
	blo	.Lbmh_panic

	/* Identity map, uncached */
	mov	r3, r1
	shr	r3, #12
	shl	r3, #12			/* PA = VA, page-aligned */
	lli	r4, #BPTE_KERNEL_NC
	or	r3, r4
	b	.Lbmh_load_tlb

.Lbmh_panic:
	break				/* unmapped address — fatal */

.Lbmh_load_tlb:
	/* R1 = faulting VA, R3 = PTE value */

	/* Build TLB_VPN = (VPN << 8) | ASID=0 */
	mov	r2, r1
	shr	r2, #12
	shl	r2, #8

	/* Compute TLB set = VPN[4:0] */
	mov	r4, r1
	shr	r4, #12
	and	r4, #0x1F

	/* Way selection: alternating counter */
	ldw	r1, [r0 + #0x58]	/* way (0 or 0x20) */
	or	r4, r1			/* TLB_INDEX = set | way */

	/* Write TLB entry (3-write commit protocol) */
	wrsys	r4, #SYSDEV_MMU, #MMU_TLB_INDEX
	wrsys	r2, #SYSDEV_MMU, #MMU_TLB_VPN
	wrsys	r3, #SYSDEV_MMU, #MMU_TLB_PTE

	/* Toggle way counter for next miss */
	lli	r1, #0x20
	ldw	r2, [r0 + #0x58]
	xor	r2, r1
	stw	r2, [r0 + #0x58]

	/* Restore scratch registers and return */
	ldw	r1, [r0 + #0x40]
	ldw	r2, [r0 + #0x44]
	ldw	r3, [r0 + #0x48]
	ldw	r4, [r0 + #0x4C]
	eret

_boot_prot_panic:
	break
_boot_handlers_end:

/*
 * cpu_switchto(oldlwp, newlwp, returning)
 * Switch context from oldlwp to newlwp.
 * Stub — will be implemented for scheduler support.
 */
	.globl	cpu_switchto
	.type	cpu_switchto, @function
cpu_switchto:
// TODO: Not implemented
	break

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
 * Early boot stack — used before MMU is enabled.
 * 4 KB is enough for the pre-MMU TLB setup code.
 */
	.bss
	.align	4
	.globl	_boot_stack
_boot_stack:
	.space	4096
	.globl	_boot_stack_top
_boot_stack_top:

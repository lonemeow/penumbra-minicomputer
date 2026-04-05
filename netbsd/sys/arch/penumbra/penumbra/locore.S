/*	$NetBSD$	*/

/*
 * Penumbra low-level kernel entry and exception handling.
 *
 * Entry point: _start (jumped to by bootloader with MMU on,
 * R1 = bootinfo pointer, running in supervisor mode).
 *
 * Exception vectors live at physical 0x0000_0000 (mapped by
 * the bootloader before jumping here):
 *   0x00: reset
 *   0x04: IRQ
 *   0x08: TLB miss
 *   0x0C: TLB protection
 *   0x10: bus fault
 *   0x14: BREAK
 *   0x18: SYSCALL
 *   0x1C: illegal / privilege
 */

#include "assym.h"

	.text
	.globl	_start
	.type	_start, @function

/*
 * Kernel entry point.
 * Called by bootloader with:
 *   R1 = bootinfo physical address
 *   SR.S = 1 (supervisor), SR.M = 1 (MMU on), SR.I = 0 (irqs off)
 *   Kernel mapped at KERNEL_TEXT_BASE via TLB
 */
_start:
// TODO: Implement proper kernel entry routine (minimal page tables, TLB, stack)
	break

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

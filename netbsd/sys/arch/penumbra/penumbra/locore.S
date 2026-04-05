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
	/* Set up kernel stack (temporary, at top of first 64K of kernel BSS) */
	LI	R14, _end
	ADD	R14, R14, 0x4000	/* 16 KB stack */

	/* Save bootinfo pointer for machdep.c */
	LI	R5, penumbra_bootinfo
	STW	R1, [R5]

	/* Clear BSS */
	LI	R1, __bss_start
	LI	R2, _end
	MOV	R3, R0			/* R0 = 0 */
1:	CMP	R1, R2
	BCS	2f			/* branch if R1 >= R2 */
	STW	R3, [R1]
	ADD	R1, R1, 4
	B	1b
2:
	/* Call C entry point */
	BL	penumbra_init
	/* NOT REACHED */
	BREAK

/*
 * Exception vector stubs.
 * These are copied to physical page 0 during boot.
 * Each vector is a single branch to the full handler.
 */
	.section .rodata
	.globl	exception_vectors
	.globl	exception_vectors_end
exception_vectors:
	B	_exc_reset		/* 0x00: reset */
	B	_exc_irq		/* 0x04: IRQ */
	B	_exc_tlb_miss		/* 0x08: TLB miss */
	B	_exc_tlb_prot		/* 0x0C: TLB protection */
	B	_exc_busfault		/* 0x10: bus fault */
	B	_exc_break		/* 0x14: BREAK */
	B	_exc_syscall		/* 0x18: SYSCALL */
	B	_exc_illegal		/* 0x1C: illegal / privilege */
exception_vectors_end:

/*
 * Exception entry — save trap frame and dispatch to C.
 *
 * On exception entry, hardware has:
 *   - Saved PC → EPC (SPR)
 *   - Saved SR → ESR (SPR)
 *   - Set SR.S = 1, SR.I = 0
 *   - Jumped to vector address
 *
 * We need to: save all regs, build trapframe, call trap().
 */
	.text

	.macro EXCEPTION_ENTRY vector_num
	/* Allocate trap frame on kernel stack */
	SUB	R14, R14, TF_SIZE

	/* Save general registers */
	STW	R0,  [R14, 0]
	STW	R1,  [R14, 4]
	STW	R2,  [R14, 8]
	STW	R3,  [R14, 12]
	STW	R4,  [R14, 16]
	STW	R5,  [R14, 20]
	STW	R6,  [R14, 24]
	STW	R7,  [R14, 28]
	STW	R8,  [R14, 32]
	STW	R9,  [R14, 36]
	STW	R10, [R14, 40]
	STW	R11, [R14, 44]
	STW	R12, [R14, 48]
	STW	R13, [R14, 52]
	/* R14 (SP) was pre-exception value + TF_SIZE */
	ADD	R11, R14, TF_SIZE
	STW	R11, [R14, 56]
	STW	R15, [R14, 60]

	/* Save EPC and ESR from SPRs */
	RDSPR	R11, epc
	STW	R11, [R14, TF_EPC]
	RDSPR	R11, esr
	STW	R11, [R14, TF_SR]

	/* Store cause vector number */
	LLI	R11, \vector_num
	STW	R11, [R14, TF_CAUSE]

	/* R1 = pointer to trapframe (first arg to trap()) */
	MOV	R1, R14
	.endm

	.macro EXCEPTION_EXIT
	/* Restore EPC and ESR */
	LDW	R11, [R14, TF_EPC]
	WRSPR	epc, R11
	LDW	R11, [R14, TF_SR]
	WRSPR	esr, R11

	/* Restore general registers */
	LDW	R0,  [R14, 0]
	LDW	R1,  [R14, 4]
	LDW	R2,  [R14, 8]
	LDW	R3,  [R14, 12]
	LDW	R4,  [R14, 16]
	LDW	R5,  [R14, 20]
	LDW	R6,  [R14, 24]
	LDW	R7,  [R14, 28]
	LDW	R8,  [R14, 32]
	LDW	R9,  [R14, 36]
	LDW	R10, [R14, 40]
	LDW	R11, [R14, 44]
	LDW	R12, [R14, 48]
	LDW	R13, [R14, 52]
	/* Restore SP from saved value */
	LDW	R14, [R14, 56]

	/* Return from exception */
	ERET
	.endm

_exc_reset:
	B	_start

_exc_irq:
	EXCEPTION_ENTRY 1
	BL	trap
	EXCEPTION_EXIT

_exc_tlb_miss:
	EXCEPTION_ENTRY 2
	/* Save faulting address */
	RDSYS	R11, 0, 1		/* FAULT_ADDR: device 0, reg 1 */
	STW	R11, [R14, TF_BADVADDR]
	BL	trap
	EXCEPTION_EXIT

_exc_tlb_prot:
	EXCEPTION_ENTRY 3
	RDSYS	R11, 0, 1
	STW	R11, [R14, TF_BADVADDR]
	BL	trap
	EXCEPTION_EXIT

_exc_busfault:
	EXCEPTION_ENTRY 4
	BL	trap
	EXCEPTION_EXIT

_exc_break:
	EXCEPTION_ENTRY 5
	BL	trap
	EXCEPTION_EXIT

_exc_syscall:
	EXCEPTION_ENTRY 6
	BL	trap
	EXCEPTION_EXIT

_exc_illegal:
	EXCEPTION_ENTRY 7
	BL	trap
	EXCEPTION_EXIT

/*
 * cpu_switchto(oldlwp, newlwp, returning)
 * Switch context from oldlwp to newlwp.
 * Stub — will be implemented for scheduler support.
 */
	.globl	cpu_switchto
	.type	cpu_switchto, @function
cpu_switchto:
	BREAK		/* not yet implemented */

/*
 * setjmp / longjmp for kernel use (label_t).
 * Save/restore callee-saved registers.
 */
	.globl	setjmp
	.type	setjmp, @function
setjmp:
	/* R1 = pointer to label_t */
	STW	R5,  [R1, 0]
	STW	R6,  [R1, 4]
	STW	R7,  [R1, 8]
	STW	R8,  [R1, 12]
	STW	R9,  [R1, 16]
	STW	R10, [R1, 20]
	STW	R12, [R1, 24]
	STW	R13, [R1, 28]
	STW	R14, [R1, 32]
	STW	R15, [R1, 36]
	MOV	R1, R0		/* return 0 */
	RET

	.globl	longjmp
	.type	longjmp, @function
longjmp:
	/* R1 = pointer to label_t */
	LDW	R5,  [R1, 0]
	LDW	R6,  [R1, 4]
	LDW	R7,  [R1, 8]
	LDW	R8,  [R1, 12]
	LDW	R9,  [R1, 16]
	LDW	R10, [R1, 20]
	LDW	R12, [R1, 24]
	LDW	R13, [R1, 28]
	LDW	R14, [R1, 32]
	LDW	R15, [R1, 36]
	LLI	R1, 1		/* return 1 */
	RET

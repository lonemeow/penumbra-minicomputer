/*	$NetBSD$	*/

#ifndef _PENUMBRA_SETJMP_H_
#define _PENUMBRA_SETJMP_H_

/*
 * jmp_buf layout:
 *   Callee-saved: R5–R10, R12(TP), R13(LR), R14(SP)
 *   Plus: PC (return address), SR, signal mask
 */
#define _JBLEN	12	/* callee-saved regs + PC + SR + sigmask */

#endif /* _PENUMBRA_SETJMP_H_ */

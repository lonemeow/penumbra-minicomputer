/*	$NetBSD$	*/

#ifndef _PENUMBRA_SETJMP_H_
#define _PENUMBRA_SETJMP_H_

/*
 * Userland jmp_buf layout (indexed by _JB_* below):
 *
 *   [0]      magic number
 *   [1-6]    callee-saved R5–R10
 *   [7]      R12 (TP)
 *   [8]      R13 (LR)
 *   [9]      R14 (SP)
 *   [10-13]  signal mask (sigset_t = 4 x uint32_t)
 *
 * No FPU — no floating-point registers to save.
 *
 * The kernel's label_t (types.h) has its own layout and indices —
 * it shares the _JB_R* naming but different offsets (no magic, no sigmask).
 */

	/* magic + 9 regs + 4 sigmask + 2 spare */
#define _JBLEN		16

#define _JB_MAGIC	0
#define _JB_R5		1
#define _JB_R6		2
#define _JB_R7		3
#define _JB_R8		4
#define _JB_R9		5
#define _JB_R10		6
#define _JB_R12		7	/* TP */
#define _JB_R13		8	/* LR */
#define _JB_R14		9	/* SP */
#define _JB_SIGMASK	10

/*
 * Magic values stored in jmp_buf[0].
 * siglongjmp checks the magic to decide whether to restore the signal mask.
 */
#define _JB_MAGIC__SETJMP	0x50454E00	/* _setjmp  (no sigmask) */
#define _JB_MAGIC_SETJMP	0x50454E01	/* __setjmp14 (sigmask saved) */

#ifndef _BSD_JBSLOT_T_
#define _BSD_JBSLOT_T_	long	/* 4 bytes on ILP32 */
#endif

#endif /* _PENUMBRA_SETJMP_H_ */

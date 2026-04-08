/*	$NetBSD$	*/

/*
 * ptrace must clear errno before the syscall.
 */

#include "SYS.h"

#define	XCALLFRAME_SIZ	(6*SZREG)
#define	XCALLFRAME_LR	(0*SZREG)
#define	XCALLFRAME_R1	(1*SZREG)
#define	XCALLFRAME_R2	(2*SZREG)
#define	XCALLFRAME_R3	(3*SZREG)
#define	XCALLFRAME_R4	(4*SZREG)

ENTRY(ptrace)
#ifdef _REENTRANT
	sub	r14, #XCALLFRAME_SIZ
	stw	r13, [r14 + XCALLFRAME_LR]
	stw	r1,  [r14 + XCALLFRAME_R1]
	stw	r2,  [r14 + XCALLFRAME_R2]
	stw	r3,  [r14 + XCALLFRAME_R3]
	stw	r4,  [r14 + XCALLFRAME_R4]

	bl	PLT(_C_LABEL(__errno))
	stw	r0, [r1]		/* *errno = 0 */

	ldw	r4,  [r14 + XCALLFRAME_R4]
	ldw	r3,  [r14 + XCALLFRAME_R3]
	ldw	r2,  [r14 + XCALLFRAME_R2]
	ldw	r1,  [r14 + XCALLFRAME_R1]
	ldw	r13, [r14 + XCALLFRAME_LR]
	add	r14, #XCALLFRAME_SIZ
#else
	la	r11, _C_LABEL(errno)
	stw	r0, [r11]
#endif
	SYSTRAP(ptrace)
	JUMP_TO_CERROR()		/* error */
	ret				/* success */
END(ptrace)

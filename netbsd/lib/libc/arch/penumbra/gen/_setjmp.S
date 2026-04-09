/*	$NetBSD$	*/

/*
 * _setjmp / _longjmp for Penumbra.
 *
 * _setjmp(env) saves callee-saved registers into jmp_buf.
 * _longjmp(env, val) restores them and returns val (or 1 if val==0).
 * Signal mask is NOT saved/restored (use __setjmp14/__longjmp14 for that).
 *
 * Penumbra ABI: R1–R4 args/scratch, R5–R10 callee-saved,
 * R12 TP, R13 LR, R14 SP, R0 zero.
 */

#include <machine/asm.h>
#include <machine/setjmp.h>

/*
 * Byte offsets into jmp_buf (slot index * 4).
 * From genassym.cf: JB_MAGIC=0, JB_R5=4, ..., JB_R14=36.
 */
#define JB_MAGIC	(_JB_MAGIC * 4)
#define JB_R5		(_JB_R5 * 4)
#define JB_R6		(_JB_R6 * 4)
#define JB_R7		(_JB_R7 * 4)
#define JB_R8		(_JB_R8 * 4)
#define JB_R9		(_JB_R9 * 4)
#define JB_R10		(_JB_R10 * 4)
#define JB_R12		(_JB_R12 * 4)
#define JB_R13		(_JB_R13 * 4)
#define JB_R14		(_JB_R14 * 4)


ENTRY(_setjmp)
	li  r2,  _JB_MAGIC__SETJMP
	stw r2,  [r1 + JB_MAGIC]
	stw r5,  [r1 + JB_R5]
	stw r6,  [r1 + JB_R6]
	stw r7,  [r1 + JB_R7]
	stw r8,  [r1 + JB_R8]
	stw r9,  [r1 + JB_R9]
	stw r10, [r1 + JB_R10]
	stw r12, [r1 + JB_R12]
	stw r13, [r1 + JB_R13]
	stw r14, [r1 + JB_R14]
	mov r1,  r0
	ret
END(_setjmp)

ENTRY(_longjmp)
	/* R1 = env, R2 = val */
	ldw	r3, [r1 + JB_MAGIC]
	li	r4, _JB_MAGIC__SETJMP
	cmp	r3, r4
	bne	.Lbad
	/* Validate LR and SP are non-zero */
	ldw	r3, [r1 + JB_R13]
	cmp	r3, r0
	beq	.Lbad
	ldw	r4, [r1 + JB_R14]
	cmp	r4, r0
	beq	.Lbad
	/* Restore SP and LR */
	mov	r13, r3
	mov	r14, r4
	/* Restore callee-saved registers */
	ldw	r5,  [r1 + JB_R5]
	ldw	r6,  [r1 + JB_R6]
	ldw	r7,  [r1 + JB_R7]
	ldw	r8,  [r1 + JB_R8]
	ldw	r9,  [r1 + JB_R9]
	ldw	r10, [r1 + JB_R10]
	ldw	r12, [r1 + JB_R12]
	/* Return val, or 1 if val == 0 */
	cmp	r2, r0
	bne	.Lexit
	li	r2, 1
.Lexit:
	mov	r1, r2
	ret
.Lbad:
	bl	PLT(_C_LABEL(longjmperror))
	bl	PLT(_C_LABEL(abort))
	b	.			/* unreachable */
END(_longjmp)

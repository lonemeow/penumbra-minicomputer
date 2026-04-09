/*	$NetBSD$	*/

/*
 * __setjmp14 / __longjmp14 for Penumbra.
 *
 * Like _setjmp/_longjmp, but also saves and restores the signal mask
 * via __sigprocmask14(2).
 */

#include <machine/asm.h>
#include <machine/setjmp.h>

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
#define JB_SIGMASK	(_JB_SIGMASK * 4)

#define SIG_SETMASK	3

/*
 * int __setjmp14(jmp_buf env)
 *
 * Save signal mask via sigprocmask, then save registers.
 * Returns 0 on direct call, non-zero on longjmp return.
 */
ENTRY(__setjmp14)
	/* Create call frame: save LR and env (R1) across sigprocmask */
	sub	r14, #CALLFRAME_SIZ
	stw	r13, [r14 + CALLFRAME_LR]
	stw	r1,  [r14 + CALLFRAME_S0]

	/* __sigprocmask14(0, NULL, &env[JB_SIGMASK]) — read current mask */
	mov	r3, r1
	add	r3, #JB_SIGMASK		/* r3 = oset = &env[JB_SIGMASK] */
	mov	r2, r0			/* r2 = set = NULL */
	mov	r1, r0			/* r1 = how = 0 (SIG_BLOCK) */
	bl	PLT(_C_LABEL(__sigprocmask14))

	/* Restore env and LR */
	ldw	r1,  [r14 + CALLFRAME_S0]
	ldw	r13, [r14 + CALLFRAME_LR]
	add	r14, #CALLFRAME_SIZ

	/* Store magic and callee-saved registers */
	li	r2, _JB_MAGIC_SETJMP
	stw	r2,  [r1 + JB_MAGIC]
	stw	r5,  [r1 + JB_R5]
	stw	r6,  [r1 + JB_R6]
	stw	r7,  [r1 + JB_R7]
	stw	r8,  [r1 + JB_R8]
	stw	r9,  [r1 + JB_R9]
	stw	r10, [r1 + JB_R10]
	stw	r12, [r1 + JB_R12]
	stw	r13, [r1 + JB_R13]
	stw	r14, [r1 + JB_R14]

	mov	r1, r0			/* return 0 */
	ret
END(__setjmp14)

/*
 * void __longjmp14(jmp_buf env, int val)
 *
 * Validate magic, restore signal mask, restore registers, return val.
 */
ENTRY(__longjmp14)
	/* R1 = env, R2 = val */
	ldw	r3, [r1 + JB_MAGIC]
	li	r4, _JB_MAGIC_SETJMP
	cmp	r3, r4
	bne	.Lbad14

	/* Validate LR and SP are non-zero */
	ldw	r3, [r1 + JB_R13]
	cmp	r3, r0
	beq	.Lbad14
	ldw	r4, [r1 + JB_R14]
	cmp	r4, r0
	beq	.Lbad14

	/* Restore SP and LR first (so we run on the setjmp caller's stack) */
	mov	r13, r3
	mov	r14, r4

	/*
	 * Save env and val on the restored stack across sigprocmask call.
	 * 12-byte frame: [SP+0]=env, [SP+4]=val, [SP+8]=LR
	 */
	sub	r14, #12
	stw	r13, [r14 + 8]
	stw	r1,  [r14 + 0]
	stw	r2,  [r14 + 4]

	/* __sigprocmask14(SIG_SETMASK, &env[JB_SIGMASK], NULL) */
	mov	r2, r1
	add	r2, #JB_SIGMASK		/* r2 = set = &env[JB_SIGMASK] */
	li	r1, #SIG_SETMASK	/* r1 = how */
	mov	r3, r0			/* r3 = oset = NULL */
	bl	PLT(_C_LABEL(__sigprocmask14))

	/* Restore env, val, LR */
	ldw	r1,  [r14 + 0]
	ldw	r2,  [r14 + 4]
	ldw	r13, [r14 + 8]
	add	r14, #12

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
	bne	.Lexit14
	li	r2, 1
.Lexit14:
	mov	r1, r2
	ret

.Lbad14:
	bl	PLT(_C_LABEL(longjmperror))
	bl	PLT(_C_LABEL(abort))
	b	.			/* unreachable */
END(__longjmp14)

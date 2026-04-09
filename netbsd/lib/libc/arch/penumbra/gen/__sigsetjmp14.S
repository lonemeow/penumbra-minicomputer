/*	$NetBSD$	*/

/*
 * __sigsetjmp14 / __siglongjmp14 for Penumbra.
 *
 * These are thin dispatchers:
 *   __sigsetjmp14(env, savemask):
 *     if savemask != 0 → tail-call __setjmp14 (saves signal mask)
 *     else             → tail-call _setjmp    (no signal mask)
 *
 *   __siglongjmp14(env, val):
 *     checks magic in jmp_buf to decide which variant to call.
 */

#include <machine/asm.h>
#include <machine/setjmp.h>

ENTRY(__sigsetjmp14)
	/* R1 = env, R2 = savemask */
	cmp	r2, r0			/* savemask == 0? */
	beq	PLT(_C_LABEL(_setjmp))
	b	PLT(_C_LABEL(__setjmp14))
END(__sigsetjmp14)

ENTRY(__siglongjmp14)
	/* R1 = env, R2 = val */
	ldw	r3, [r1 + 0]		/* jmp_buf[0] = magic */
	li	r4, #_JB_MAGIC__SETJMP
	cmp	r3, r4
	beq	PLT(_C_LABEL(_longjmp))
	b	PLT(_C_LABEL(__longjmp14))
END(__siglongjmp14)

/*	$NetBSD$	*/

/*
 * int __clone(int (*fn)(void *), void *stack, int flags, void *arg);
 *   R1 = fn, R2 = stack, R3 = flags, R4 = arg
 */

#include <sys/errno.h>
#include "SYS.h"

#ifdef WEAK_ALIAS
WEAK_ALIAS(clone, __clone)
#endif

ENTRY(__clone)
	/* Sanity checks: fn and stack may not be NULL */
	mov	r11, r1			/* save fn (r1 will be clobbered) */
	lli	r1, EINVAL
	test	r11, r11
	beq	8f
	test	r2, r2
	beq	8f

	/* Stash fn and arg on child's stack */
	sub	r2, #8
	stw	r11, [r2 + 0]		/* fn */
	stw	r4,  [r2 + 4]		/* arg */

	/* Syscall expects (flags, stack) in R1, R2 */
	mov	r1, r3			/* flags → R1 */
	/* R2 = stack already */
	lli	r1, SYS___clone
	syscall

8:	bcs	_C_LABEL(__cerror)	/* error */
	test	r2, r2			/* r2 (rval[1]): 0=parent, 1=child */
	bne	9f
	ret				/* parent: return child pid in r1 */

9:	/* Child: fetch fn and arg from stack */
	ldw	r11, [r14 + 0]		/* fn */
	ldw	r1,  [r14 + 4]		/* arg */

	/* Call fn(arg) */
	mov	r13, r0			/* clear LR (end of stack trace) */
	bl	_indirect_r11

	/* fn returned — call _exit(retval) */
	b	_C_LABEL(_exit)

_indirect_r11:
	jmp	r11
END(__clone)

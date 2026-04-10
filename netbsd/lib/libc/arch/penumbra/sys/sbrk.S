/*	$NetBSD$	*/

#include "SYS.h"

	.hidden	_C_LABEL(__curbrk)
	.globl	_C_LABEL(__curbrk)

#ifdef WEAK_ALIAS
WEAK_ALIAS(sbrk, _sbrk)
#endif

ENTRY(_sbrk)
	lli	r3, %lo16(_C_LABEL(__curbrk))
	lui	r3, %hi16(_C_LABEL(__curbrk))
	ldw	r4, [r3]		/* R4 = old __curbrk */
	add	r1, r4			/* new break = curbrk + increment */
	SYSTRAP(break)
	JUMP_TO_CERROR()		/* error */
	stw	r1, [r3]		/* update __curbrk (R3 preserved) */
	mov	r1, r4			/* return old curbrk (R4 preserved) */
	ret
END(_sbrk)

/*	$NetBSD$	*/

#include "SYS.h"

	.hidden	_C_LABEL(__curbrk)
	.globl	_C_LABEL(__curbrk)

#ifdef WEAK_ALIAS
WEAK_ALIAS(sbrk, _sbrk)
#endif

ENTRY(_sbrk)
	mov	r11, r15
	add	r11, %pcrel(_C_LABEL(__curbrk)+4)
	ldw	r2, [r11]
	add	r1, r2			/* new break = curbrk + increment */
	SYSTRAP(break)
	JUMP_TO_CERROR()		/* error */
	stw	r1, [r11]		/* update __curbrk */
	mov	r1, r2			/* return old curbrk */
	ret
END(_sbrk)

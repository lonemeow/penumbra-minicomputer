/*	$NetBSD$	*/

#include "SYS.h"

	.hidden	_C_LABEL(__minbrk)
	.hidden	_C_LABEL(__curbrk)
	.globl	_C_LABEL(__minbrk)
	.globl	_C_LABEL(__curbrk)
	.globl	_C_LABEL(_end)

#ifdef WEAK_ALIAS
WEAK_ALIAS(brk, _brk)
#endif

	.data
	.p2align 2
_C_LABEL(__minbrk):
	.long	_C_LABEL(_end)
_C_LABEL(__curbrk):
	.long	_C_LABEL(_end)

	.text

ENTRY(_brk)
	lli	r11, %lo16(_C_LABEL(__minbrk))
	lui	r11, %hi16(_C_LABEL(__minbrk))
	ldw	r2, [r11 + 0]
	cmp	r1, r2
	bhs	1f
	mov	r1, r2			/* don't allow break < minbrk */
1:
	SYSTRAP(break)
	JUMP_TO_CERROR()		/* error */
	lli	r11, %lo16(_C_LABEL(__curbrk))
	lui	r11, %hi16(_C_LABEL(__curbrk))
	stw	r1, [r11 + 0]		/* update __curbrk */
	mov	r1, r0			/* return 0 */
	ret
END(_brk)

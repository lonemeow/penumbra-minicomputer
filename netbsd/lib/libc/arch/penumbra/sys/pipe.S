/*	$NetBSD$	*/

/*
 * int pipe(int fildes[2]);
 * R1 = fildes pointer.
 * Kernel returns two fds in R1 and R2.
 */

#include "SYS.h"

#ifdef WEAK_ALIAS
WEAK_ALIAS(pipe, _pipe)
#endif

ENTRY(_pipe)
	mov	r11, r1			/* save fildes pointer */
	SYSTRAP(pipe)
	JUMP_TO_CERROR()		/* error */
	stw	r1, [r11 + 0]		/* fildes[0] */
	stw	r2, [r11 + 4]		/* fildes[1] */
	mov	r1, r0			/* return 0 */
	ret
END(_pipe)

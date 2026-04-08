/*	$NetBSD$	*/

/*
 * pid = vfork();
 * Same parent/child convention as fork.
 */

#include "SYS.h"

ENTRY(__vfork14)
	SYSTRAP(__vfork14)
	JUMP_TO_CERROR()		/* error */
	sub	r2, #1			/* parent: 0xFFFFFFFF, child: 0 */
	and	r1, r2			/* parent: pid, child: 0 */
	ret
END(__vfork14)

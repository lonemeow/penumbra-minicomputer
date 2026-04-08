/*	$NetBSD$	*/

/*
 * pid = fork();
 *
 * After the syscall, the kernel returns:
 *   R1 = pid, R2 = 0 in parent
 *   R1 = pid, R2 = 1 in child
 * We need to return child_pid in parent, 0 in child.
 */

#include "SYS.h"

ENTRY(__fork)
	SYSTRAP(fork)
	JUMP_TO_CERROR()		/* error */
	/* success: r2=0 parent, r2=1 child */
	sub	r2, #1			/* parent: r2=0xFFFFFFFF, child: r2=0 */
	and	r1, r2			/* parent: r1=pid, child: r1=0 */
	ret
END(__fork)

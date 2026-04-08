/*	$NetBSD$	*/

/*
 * Indirect syscall.  On Penumbra, the calling convention already
 * has R1=number, R2-R4=args, matching the kernel's expectation.
 * Note: the kernel currently rejects SYS___syscall with ENOSYS.
 */

#include "SYS.h"

ENTRY(__syscall)
	SYSTRAP(__syscall)
	JUMP_TO_CERROR()
	ret
END(__syscall)
STRONG_ALIAS(_syscall,__syscall)
WEAK_ALIAS(syscall,__syscall)

/*	$NetBSD$	*/

/*
 * getcontext must adjust the saved context after the syscall
 * so that setcontext() resumes at the right place.
 */

#include "SYS.h"
#include "assym.h"

#ifdef WEAK_ALIAS
WEAK_ALIAS(getcontext, _getcontext)
#endif

ENTRY(_getcontext)
	mov	r3, r1				/* save ucp in R3 */
	SYSTRAP(getcontext)
	JUMP_TO_CERROR()			/* error */
	stw	r0,  [r3 + UC_GREGS_RV]	/* retval = 0 on resume */
	stw	r13, [r3 + UC_GREGS_PC]		/* resume at caller */
	ret
END(_getcontext)

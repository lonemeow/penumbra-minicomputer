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
	SYSTRAP(getcontext)
	JUMP_TO_CERROR()			/* error */
	stw	r0,  [r1 + UC_GREGS_RV]	/* retval = 0 on resume */
	stw	r13, [r1 + UC_GREGS_PC]		/* resume at caller */
	ret
END(_getcontext)

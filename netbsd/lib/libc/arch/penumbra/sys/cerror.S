/*	$NetBSD$	*/

/*
 * __cerror — syscall error handler for Penumbra.
 *
 * Called (via BCS) when a syscall returns with carry set.
 * R1 contains the errno value from the kernel.
 * Stores it in errno (thread-local if _REENTRANT) and returns -1.
 */

#include "SYS.h"

	.protected	_C_LABEL(__cerror)

	.globl	_C_LABEL(__errno)
ENTRY_NP(__cerror)
#ifdef _REENTRANT
	sub	r14, #CALLFRAME_SIZ
	stw	r13, [r14 + CALLFRAME_LR]	/* save return address */
	stw	r1,  [r14 + CALLFRAME_S0]	/* save errno value */

	bl	PLT(_C_LABEL(__errno))		/* r1 = &errno */

	ldw	r2,  [r14 + CALLFRAME_S0]	/* retrieve errno value */
	ldw	r13, [r14 + CALLFRAME_LR]	/* retrieve return address */
	stw	r2,  [r1]			/* *__errno() = saved errno */
	add	r14, #CALLFRAME_SIZ
#else
	/* Non-reentrant: store directly to global errno */
	la	r2, _C_LABEL(errno)
	stw	r1, [r2]
#endif /* _REENTRANT */
	li	r1, #-1
	ret
END(__cerror)

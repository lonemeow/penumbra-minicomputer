/*	$NetBSD$	*/

/*
 * Signal trampoline for Penumbra.
 *
 * The kernel's sendsig_siginfo() arranges for the signal handler to
 * be called with:
 *   R1 = signal number
 *   R2 = pointer to siginfo_t
 *   R3 = pointer to ucontext_t
 *   R5 = pointer to ucontext_t (callee-saved copy)
 *   LR = address of this trampoline
 *
 * When the handler returns, execution lands here.  We call
 * setcontext(uc) to restore the interrupted context.  If that
 * fails (shouldn't happen), exit with the errno.
 */

#include "SYS.h"

ENTRY(__sigtramp_siginfo_2)
	mov	r1, r5			/* ucontext pointer (saved by kernel) */
	SYSTRAP(setcontext)		/* restore machine state */
	SYSTRAP(exit)			/* if setcontext failed, exit */
END(__sigtramp_siginfo_2)

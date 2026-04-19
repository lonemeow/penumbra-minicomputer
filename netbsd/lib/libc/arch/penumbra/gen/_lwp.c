/*	$NetBSD$	*/

#include <sys/cdefs.h>
#if defined(LIBC_SCCS) && !defined(lint)
__RCSID("$NetBSD$");
#endif

#include "namespace.h"
#include <sys/types.h>
#include <sys/tls.h>
#include <ucontext.h>
#include <lwp.h>
#include <stdlib.h>

void
_lwp_makecontext(ucontext_t *u, void (*start)(void *),
    void *arg, void *private, caddr_t stack_base, size_t stack_size)
{
	uintptr_t sp;

	getcontext(u);
	u->uc_link = NULL;

	u->uc_stack.ss_sp = stack_base;
	u->uc_stack.ss_size = stack_size;

	sp = (uintptr_t)stack_base + stack_size;
	sp &= ~7;	/* 8-byte aligned */

	u->uc_mcontext.__gregs[1]         = (__greg_t)(uintptr_t)arg;    /* R1 = arg */
	u->uc_mcontext.__gregs[_REG_SP]   = (__greg_t)sp;
	u->uc_mcontext.__gregs[_REG_LR]   = (__greg_t)(uintptr_t)_lwp_exit;
	u->uc_mcontext.__gregs[_REG_PC]   = (__greg_t)(uintptr_t)start;
	u->uc_mcontext.__gregs[_REG_R12]  = (__greg_t)(uintptr_t)private
	    + TLS_TP_OFFSET + sizeof(struct tls_tcb);               /* R12 = TP */
	u->uc_flags |= _UC_TLSBASE;
}

/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent pthread definitions.
 *
 * The stack pointer, live and as saved in a ucontext, is all libpthread
 * asks of this header: threads are kernel LWPs, created through
 * _lwp_makecontext and _lwp_create, so nothing switches context in
 * user space.
 */

#ifndef _LIB_PTHREAD_PENUMBRA_MD_H
#define _LIB_PTHREAD_PENUMBRA_MD_H

static inline unsigned long
pthread__sp(void)
{
	unsigned long ret;
	__asm("mov %0, r14" : "=r" (ret));
	return ret;
}

#define pthread__uc_sp(ucp) ((ucp)->uc_mcontext.__gregs[14])	/* R14=SP */

#endif /* _LIB_PTHREAD_PENUMBRA_MD_H */

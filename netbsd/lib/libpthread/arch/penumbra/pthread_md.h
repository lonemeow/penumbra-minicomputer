/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent pthread definitions.
 *
 * Minimal implementation — enough for libpthread to compile and link.
 * Full thread support requires makecontext/setcontext (not yet implemented).
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

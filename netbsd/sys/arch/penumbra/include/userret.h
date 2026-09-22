/*	$NetBSD$	*/

#ifndef _PENUMBRA_USERRET_H_
#define _PENUMBRA_USERRET_H_

#include <sys/userret.h>
#include <sys/ras.h>

#include <machine/frame.h>

static __inline void
userret(struct lwp *l, struct trapframe *tf)
{
	struct proc * const p = l->l_proc;

	mi_userret(l);

	/*
	 * Check if a RAS (Restartable Atomic Sequence) was interrupted.
	 * If so, restart from the beginning of the sequence.
	 *
	 * The list is never empty here: libc registers the atomic_cas_32
	 * sequence in every process, so this walks a list and brackets it
	 * with the preemption counter on every return this path serves.
	 */
	if (__predict_false(p->p_raslist != NULL)) {
		void * const ras_pc = ras_lookup(p,
		    (void *)(uintptr_t)tf->tf_epc);
		if (ras_pc != (void *)-1)
			tf->tf_epc = (uint32_t)(uintptr_t)ras_pc;
	}
}

/*
 * Return to userland from a system call.  A restartable sequence may
 * invoke neither functions nor system calls (rasctl(2)), so a trapframe
 * that entered the kernel through one cannot hold a PC inside a
 * registered range, and the lookup userret() does would always miss.
 */
static __inline void
userret_syscall(struct lwp *l)
{

	mi_userret(l);
}

#endif /* _PENUMBRA_USERRET_H_ */

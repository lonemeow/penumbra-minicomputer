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
	 */
	if (__predict_false(p->p_raslist != NULL)) {
		void * const ras_pc = ras_lookup(p,
		    (void *)(uintptr_t)tf->tf_epc);
		if (ras_pc != (void *)-1)
			tf->tf_epc = (uint32_t)(uintptr_t)ras_pc;
	}
}

#endif /* _PENUMBRA_USERRET_H_ */

/*	$NetBSD$	*/

#ifndef _PENUMBRA_PCB_H_
#define _PENUMBRA_PCB_H_

#include <machine/frame.h>
#include <machine/types.h>

/*
 * Penumbra process control block.
 * Stored in the lwp structure; holds context for switching.
 */

struct pcb {
	/*
	 * Kernel context for cpu_switchto().
	 * Saves callee-saved registers: R5–R10, R12(TP), R13(LR),
	 * R14(SP), R15(PC), plus SR.
	 */
	label_t		pcb_context;

	/* On-fault handler for copyin/copyout */
	void		*pcb_onfault;

	/* Saved user-mode SP (for kernel entry from user mode) */
	vaddr_t		pcb_usp;
};

#endif /* _PENUMBRA_PCB_H_ */

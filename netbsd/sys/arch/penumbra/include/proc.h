/*	$NetBSD$	*/

#ifndef _PENUMBRA_PROC_H_
#define _PENUMBRA_PROC_H_

/*
 * Machine-dependent per-LWP and per-process state.
 */

struct mdlwp {
	struct trapframe *md_utf;	/* user trap frame */
	vaddr_t		md_ss_addr;	/* single-step breakpoint addr */
	int		md_ss_instr;	/* saved instruction at ss addr */
	volatile int	md_astpending;	/* AST pending flag */
};

struct mdproc {
	void		(*md_syscall)(struct trapframe *);
	int		md_flags;
#define MDP_SYSCALL	0x0001		/* has used SYSCALL instruction */
};

#endif /* _PENUMBRA_PROC_H_ */

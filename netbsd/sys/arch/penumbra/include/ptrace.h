/*	$NetBSD$	*/

#ifndef _PENUMBRA_PTRACE_H_
#define _PENUMBRA_PTRACE_H_

/*
 * Machine-dependent ptrace definitions.
 */

#include <machine/reg.h>

#define PT_GETREGS	(PT_FIRSTMACH + 0)
#define PT_SETREGS	(PT_FIRSTMACH + 1)

#define PT_MACHDEP_STRINGS \
	"PT_GETREGS", \
	"PT_SETREGS",

/* Register accessors for ptrace */
#define PTRACE_REG_PC(r)	(r)->r_pc
#define PTRACE_REG_FP(r)	0		/* no dedicated FP register */
#define PTRACE_REG_SET_PC(r, v)	(r)->r_pc = (v)
#define PTRACE_REG_SP(r)	(r)->r_regs[14]
#define PTRACE_REG_INTRV(r)	(r)->r_regs[1]

#define PTRACE_BREAKPOINT	((const uint8_t[]) { 0, 0, 0, 0 })
#define PTRACE_BREAKPOINT_SIZE	4

#ifdef _KERNEL
int	process_read_regs(struct lwp *, struct reg *);
int	process_write_regs(struct lwp *, const struct reg *);
#endif

#endif /* _PENUMBRA_PTRACE_H_ */

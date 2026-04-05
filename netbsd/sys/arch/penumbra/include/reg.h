/*	$NetBSD$	*/

#ifndef _PENUMBRA_REG_H_
#define _PENUMBRA_REG_H_

/*
 * Register set definitions for ptrace and coredumps.
 */

struct reg {
	uint32_t r_regs[16];	/* R0–R15 */
	uint32_t r_sr;		/* status register */
	uint32_t r_pc;		/* program counter (EPC on trap) */
};

/* No FPU — Penumbra has no floating point hardware */
struct fpreg {
	int	dummy;		/* placeholder */
};

#endif /* _PENUMBRA_REG_H_ */

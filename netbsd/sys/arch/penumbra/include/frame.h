/*	$NetBSD$	*/

#ifndef _PENUMBRA_FRAME_H_
#define _PENUMBRA_FRAME_H_

/*
 * Penumbra trap frame — saved on exception entry.
 *
 * Register layout matches the ISA:
 *   R0 (zero), R1–R4 (args/retval), R5–R10 (callee-saved),
 *   R11 (scratch), R12 (TP), R13 (LR), R14 (SP), R15 (PC)
 *
 * Exception entry saves: all GPRs, SR, EPC (saved PC),
 * and the cause/fault info.
 */

struct trapframe {
	uint32_t tf_regs[16];	/* R0–R15 */
	uint32_t tf_sr;		/* status register */
	uint32_t tf_epc;	/* saved PC (from EPC SPR) */
	uint32_t tf_cause;	/* exception cause vector number */
	uint32_t tf_badvaddr;	/* faulting address (for MMU faults) */
};

/* Register indices into tf_regs[] */
#define TF_R0		0
#define TF_R1		1	/* return value / arg0 */
#define TF_R2		2
#define TF_R3		3
#define TF_R4		4
#define TF_R11		11	/* scratch */
#define TF_R12		12	/* thread pointer */
#define TF_R13		13	/* link register */
#define TF_R14		14	/* stack pointer */
#define TF_R15		15	/* program counter */

/* Size of trapframe in bytes (for assembly) */
#define TF_SIZE		(20 * 4)

#endif /* _PENUMBRA_FRAME_H_ */

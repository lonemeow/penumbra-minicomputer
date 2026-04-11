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
	uint32_t tf_fault_status; /* MMU_FAULT_STATUS (access type + fault info) */
};

/* Register indices into tf_regs[] */
#define TF_R0		0
#define TF_R1		1	/* return value / arg0 */
#define TF_R2		2	/* arg1 / rval[1] */
#define TF_R3		3	/* arg2 */
#define TF_R4		4	/* arg3 */
#define TF_R5		5	/* callee-saved */
#define TF_R6		6	/* callee-saved */
#define TF_R7		7	/* callee-saved */
#define TF_R8		8	/* callee-saved */
#define TF_R9		9	/* callee-saved */
#define TF_R10		10	/* callee-saved */
#define TF_R11		11	/* scratch / syscall number */
#define TF_R12		12	/* thread pointer (TP) */
#define TF_R13		13	/* link register (LR) */
#define TF_R14		14	/* stack pointer (SP) */
#define TF_R15		15	/* program counter (PC) */

/* Semantic aliases */
#define TF_SP		TF_R14
#define TF_LR		TF_R13
#define TF_TP		TF_R12

/* Size of trapframe in bytes (for assembly) */
#define TF_SIZE		(21 * 4)

#endif /* _PENUMBRA_FRAME_H_ */

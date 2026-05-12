/*	$NetBSD$	*/

#ifndef _PENUMBRA_DB_MACHDEP_H_
#define _PENUMBRA_DB_MACHDEP_H_

/*
 * Machine-dependent definitions for DDB (kernel debugger).
 */

#include <machine/frame.h>
#include <machine/types.h>

typedef vaddr_t		db_addr_t;
typedef long		db_expr_t;

#define DDB_EXPR_FMT	"l"		/* db_expr_t is long; MI appends x/o/u/d */

typedef struct trapframe db_regs_t;

extern db_regs_t	ddb_regs;
#define DDB_REGS	(&ddb_regs)

int	kdb_trap(int, struct trapframe *);

#define PC_REGS(r)		((r)->tf_epc)
#define PC_ADVANCE(r)		((r)->tf_epc += 4)

/*
 * BREAK instruction encoding placeholder.  We don't plant software
 * breakpoints (no single-step, no `b <addr>`), so this only needs to
 * compile.  Update if/when breakpoint planting is added.
 */
#define BKPT_INST		0x00000000
#define BKPT_SIZE		4
#define BKPT_SET(inst, addr)	BKPT_INST
#define BKPT_ADDR(addr)		(addr)

/* EXC_BREAK == 6 (VEC_BREAK, arch.md:312); was incorrectly 5 in the stub. */
#define IS_BREAKPOINT_TRAP(type, code)	((type) == 6)
#define IS_WATCHPOINT_TRAP(type, code)	0

/*
 * Instruction predicates — only consulted by single-step / breakpoint
 * code paths, which we don't enable.  Stubbed to 0; the unwinder in
 * db_trace.c uses its own prologue-pattern matchers below.
 *
 * Macros must reference `ins' so MI code (db_run.c) doesn't warn
 * about an unused variable when all predicates fold to a constant.
 */
#define inst_trap_return(ins)	((void)(ins), 0)
#define inst_return(ins)	((void)(ins), 0)
#define inst_call(ins)		((void)(ins), 0)
#define inst_branch(ins)	((void)(ins), 0)
#define inst_load(ins)		((void)(ins), 0)
#define inst_store(ins)		((void)(ins), 0)
#define inst_unconditional_flow_transfer(ins) ((void)(ins), 0)

/*
 * Prologue instruction recognition (used by db_trace.c).
 *
 * Format L "SUB Rd, #imm16":
 *   bits 31:30 = 01 (Format L)
 *   bits 29:26 = 0100 (op = SUB)
 *   bits 25:22 = Rd
 *   bits 21:16 = spare
 *   bits 15:0  = unsigned imm16
 * For Rd = R14 the upper 10 bits are 01 0100 1110 = 0x153.
 *
 * Format M "STW Rd, [Rb, #off16]":
 *   bits 31:30 = 10 (Format M)
 *   bit  29    = 0  (L=0, store)
 *   bits 28:27 = 10 (sz = word)
 *   bit  26    = SE (don't-care for stores)
 *   bits 25:22 = Rd (source register)
 *   bits 21:18 = Rb (base register)
 *   bits 17:2  = signed 16-bit byte offset
 *   bits 1:0   = spare
 * For Rb = R14 the relevant fixed bits are
 *   31:30 = 10, 29 = 0, 28:27 = 10, 21:18 = 1110.
 *
 * See doc/system/instruction-encoding.md.
 */
#define PENUMBRA_INSN_SUBI_R14_MASK	0xFFC00000u
#define PENUMBRA_INSN_SUBI_R14_MATCH	0x41800000u
#define PENUMBRA_INSN_SUBI_IMM(insn)	((insn) & 0xFFFFu)

#define PENUMBRA_INSN_STW_R14_MASK	0xF83C0000u
#define PENUMBRA_INSN_STW_R14_MATCH	0x90380000u
#define PENUMBRA_INSN_STW_RD(insn)	(((insn) >> 22) & 0xFu)
#define PENUMBRA_INSN_STW_OFF(insn) \
	((int32_t)((int16_t)(((insn) >> 2) & 0xFFFFu)))

/* No MD ddb commands yet; do not define DB_MACHINE_COMMANDS. */

/*
 * Single-step is not implemented (Penumbra has no trace-trap bit).
 * Stub macros let MI db_run.c link; we never enter SOFTWARE_SSTEP
 * paths because the predicates above all evaluate to 0.
 */
#define db_set_single_step(r)	((void)(r))
#define db_clear_single_step(r)	((void)(r))

#endif /* _PENUMBRA_DB_MACHDEP_H_ */

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

#define DDB_EXPR_FMT	"lx"
#define DDB_ADDR_FMT	PRIxPTR

typedef struct trapframe db_regs_t;

extern db_regs_t	ddb_regs;
#define DDB_REGS	(&ddb_regs)

#define PC_REGS(r)		((r)->tf_epc)
#define PC_ADVANCE(r)		((r)->tf_epc += 4)

/* BREAK instruction encoding for DDB breakpoints */
#define BKPT_INST		0x00000000	/* TODO: actual BREAK encoding */
#define BKPT_SIZE		4
#define BKPT_SET(inst, addr)	BKPT_INST
#define BKPT_ADDR(addr)		(addr)

#define IS_BREAKPOINT_TRAP(type, code)	((type) == 5)	/* VEC_BREAK */
#define IS_WATCHPOINT_TRAP(type, code)	0

#define inst_trap_return(ins)	0
#define inst_return(ins)	0	/* TODO: detect RET pseudo-op */
#define inst_call(ins)		0	/* TODO: detect BL instruction */
#define inst_branch(ins)	0	/* TODO: detect branch instructions */
#define inst_load(ins)		0	/* TODO: detect load instructions */
#define inst_store(ins)		0	/* TODO: detect store instructions */
#define inst_unconditional_flow_transfer(ins) 0

/* DDB command table (no MD commands yet) */
#define DB_MACHINE_COMMANDS

#endif /* _PENUMBRA_DB_MACHDEP_H_ */

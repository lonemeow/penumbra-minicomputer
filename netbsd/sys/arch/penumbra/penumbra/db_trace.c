/*	$NetBSD$	*/

/*
 * Penumbra DDB stack unwinder.
 *
 * Strategy: prologue-scan.  Penumbra has no frame pointer convention
 * (R10 may be one but the compiler usually omits it), so to walk a
 * frame we locate the enclosing function via the symbol table, scan
 * its prologue for the SP adjustment and the saved-LR slot, then
 * step up.
 *
 * Two seed paths:
 *   - default            from ddb_regs (the trapframe DDB entered on)
 *   - "bt /t <lwp_addr>" from the LWP's saved pcb_context — used to
 *                        inspect parked or otherwise non-running LWPs.
 *
 * Termination: we stop on bad SP, unknown symbol, implausible frame
 * size, or when we enter hand-written asm (vector page, cpu_switchto,
 * lwp_trampoline, _trap_common, trap_return) whose prologues this
 * scanner cannot interpret.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/lwp.h>

#include <machine/db_machdep.h>
#include <machine/frame.h>
#include <machine/pcb.h>
#include <machine/psl.h>

#include <ddb/db_access.h>
#include <ddb/db_extern.h>
#include <ddb/db_interface.h>
#include <ddb/db_output.h>
#include <ddb/db_proc.h>
#include <ddb/db_sym.h>

#define	MAX_FRAMES		64
#define	MAX_PROLOGUE_INSNS	16
#define	MAX_FRAME_SIZE		4096

struct unwind_state {
	uint32_t	pc;	/* current frame PC */
	uint32_t	sp;	/* current frame SP (entry SP of this fn) */
	uint32_t	lr;	/* live LR; valid only when lr_live is true */
	bool		lr_live;
};

struct prologue_info {
	uint32_t	frame_size;
	int32_t		lr_offset;	/* -1 if LR not saved */
	bool		valid;		/* false = no SUB sp found */
};

static bool
is_kernel_va(uint32_t va)
{

	return (va & 0x80000000u) != 0;
}

static bool
is_pinned_va(uint32_t va)
{

	return (va & 0xFFFF0000u) == 0xFFFF0000u;
}

static bool
read_u32(uint32_t va, uint32_t *out)
{

	if (!is_kernel_va(va))
		return false;
	db_read_bytes(va, sizeof(*out), (char *)out);
	return true;
}

static bool
match_callee_save_stw(uint32_t insn, unsigned int *reg_out, int32_t *off_out)
{
	unsigned int rd;

	if ((insn & PENUMBRA_INSN_STW_R14_MASK) != PENUMBRA_INSN_STW_R14_MATCH)
		return false;

	rd = PENUMBRA_INSN_STW_RD(insn);
	switch (rd) {
	case 5: case 6: case 7: case 8: case 9: case 10:	/* callee-saved */
	case 13:						/* LR */
		*reg_out = rd;
		*off_out = PENUMBRA_INSN_STW_OFF(insn);
		return true;
	default:
		return false;
	}
}

/*
 * Scan a function's prologue starting at func_start, up to either
 * pc_in_fn or MAX_PROLOGUE_INSNS, whichever comes first.  Record the
 * frame size from the single SUB r14, #imm and the offset of any
 * STW r13, [r14, #off] that we see.
 */
static bool
scan_prologue(db_addr_t func_start, db_addr_t pc_in_fn,
    struct prologue_info *out)
{
	uint32_t insn;
	db_addr_t scan_end;
	db_addr_t va;
	bool saw_sub;

	out->frame_size = 0;
	out->lr_offset = -1;
	out->valid = false;

	scan_end = func_start + MAX_PROLOGUE_INSNS * 4;
	if (pc_in_fn < scan_end)
		scan_end = pc_in_fn + 4;	/* include pc_in_fn itself */
	saw_sub = false;

	for (va = func_start; va < scan_end; va += 4) {
		unsigned int reg;
		int32_t off;

		if (!read_u32(va, &insn))
			return false;

		if ((insn & PENUMBRA_INSN_SUBI_R14_MASK) ==
		    PENUMBRA_INSN_SUBI_R14_MATCH) {
			if (saw_sub)
				break;	/* second SUB — not prologue */
			out->frame_size = PENUMBRA_INSN_SUBI_IMM(insn);
			saw_sub = true;
			continue;
		}

		if (match_callee_save_stw(insn, &reg, &off)) {
			if (reg == 13)	/* LR */
				out->lr_offset = off;
			continue;
		}

		/* End of prologue (or not a prologue pattern). */
		break;
	}

	out->valid = saw_sub;
	return true;
}

/*
 * Recognize symbols whose hand-written prologues this scanner cannot
 * interpret.  Returning non-NULL means "stop here with a marker"; the
 * caller does not try to step past these.
 */
static const char *
asm_boundary_name(db_addr_t pc)
{
	db_sym_t sym;
	const char *name;
	db_expr_t off;

	if (is_pinned_va(pc))
		return "asm: vector page";

	sym = db_search_symbol(pc, DB_STGY_PROC, &off);
	if (sym == DB_SYM_NULL)
		return NULL;
	db_symbol_values(sym, &name, NULL);
	if (name == NULL)
		return NULL;

	if (strcmp(name, "cpu_switchto") == 0 ||
	    strcmp(name, "lwp_trampoline") == 0 ||
	    strcmp(name, "_trap_common") == 0 ||
	    strcmp(name, "trap_return") == 0 ||
	    strcmp(name, "setjmp") == 0 ||
	    strcmp(name, "longjmp") == 0)
		return name;

	return NULL;
}

static bool
step_frame(struct unwind_state *st, void (*pr)(const char *, ...))
{
	db_sym_t sym;
	const char *name;
	db_expr_t off, fn_value;
	struct prologue_info pi;
	uint32_t ra, new_sp;

	sym = db_search_symbol(st->pc, DB_STGY_PROC, &off);
	if (sym == DB_SYM_NULL) {
		(*pr)("    0x%08x   <no symbol>\n", st->pc);
		return false;
	}
	db_symbol_values(sym, &name, &fn_value);
	(*pr)("    0x%08x   %s+0x%lx\n", st->pc,
	    name ? name : "?", (unsigned long)off);

	if (!scan_prologue((db_addr_t)fn_value, (db_addr_t)st->pc, &pi))
		return false;

	if (!pi.valid) {
		/*
		 * No SUB r14 found — function is a leaf (no frame).
		 * Only safe at the top frame, where LR is still live.
		 */
		if (!st->lr_live)
			return false;
		ra = st->lr;
		new_sp = st->sp;
	} else {
		if (pi.frame_size < 4 || pi.frame_size > MAX_FRAME_SIZE)
			return false;
		if (pi.lr_offset >= 0) {
			uint32_t slot;

			slot = st->sp + (uint32_t)pi.lr_offset;
			if (!read_u32(slot, &ra))
				return false;
		} else {
			/*
			 * Frame established but LR not saved — must be a
			 * leaf that uses locals.  Only legitimate at the
			 * topmost frame.
			 */
			if (!st->lr_live)
				return false;
			ra = st->lr;
		}
		new_sp = st->sp + pi.frame_size;
	}

	st->pc = ra;
	st->sp = new_sp;
	st->lr_live = false;
	return true;
}

static bool
seed_from_trapframe(const struct trapframe *tf, struct unwind_state *st)
{

	st->pc = tf->tf_epc;
	st->sp = tf->tf_regs[TF_SP];
	st->lr = tf->tf_regs[TF_LR];
	st->lr_live = true;
	return is_kernel_va(st->pc) && is_kernel_va(st->sp);
}

static bool
seed_from_lwp(struct lwp *l, struct unwind_state *st)
{
	struct pcb *pcb;

	if (l == NULL)
		return false;
	pcb = lwp_getpcb(l);
	if (pcb == NULL)
		return false;

	/*
	 * cpu_switchto saved this LWP's callee-saved context.  The saved
	 * R13 (LR) is the return address out of cpu_switchto — i.e. into
	 * mi_switch — so the seeded frame is already one above the asm
	 * boundary.  No need to model the cpu_switchto frame itself.
	 */
	st->pc = pcb->pcb_context.val[_JB_R13];
	st->sp = pcb->pcb_context.val[_JB_R14];
	st->lr = 0;
	st->lr_live = false;
	return is_kernel_va(st->pc) && is_kernel_va(st->sp);
}

void
db_stack_trace_print(db_expr_t addr, bool have_addr,
    db_expr_t count, const char *modif,
    void (*pr)(const char *, ...) __printflike(1, 2))
{
	struct unwind_state st;
	bool trace_lwp = false;
	const char *p;
	int limit, frames;

	for (p = modif; p != NULL && *p != '\0'; p++) {
		if (*p == 't')
			trace_lwp = true;
		else if (*p == 'u') {
			(*pr)("user-stack trace not supported\n");
			return;
		}
	}

	if (trace_lwp) {
		if (!have_addr) {
			(*pr)("bt /t requires an lwp address\n");
			return;
		}
		if (!seed_from_lwp((struct lwp *)(uintptr_t)addr, &st)) {
			(*pr)("invalid lwp pointer or unswitched lwp\n");
			return;
		}
	} else {
		if (!seed_from_trapframe(&ddb_regs, &st)) {
			(*pr)("trapframe pc/sp not in kernel range\n");
			return;
		}
	}

	limit = (count > 0 && count < MAX_FRAMES) ? (int)count : MAX_FRAMES;

	for (frames = 0; frames < limit; frames++) {
		const char *bname;

		if (!is_kernel_va(st.pc)) {
			if (st.pc == 0)
				break;
			(*pr)("    0x%08x   <non-kernel>\n", st.pc);
			break;
		}

		bname = asm_boundary_name(st.pc);
		if (bname != NULL) {
			(*pr)("    0x%08x   <%s>\n", st.pc, bname);
			break;
		}

		if (!step_frame(&st, pr))
			break;
	}
}

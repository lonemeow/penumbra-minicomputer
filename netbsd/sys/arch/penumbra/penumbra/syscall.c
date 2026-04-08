/*	$NetBSD$	*/

/*
 * Penumbra system call dispatch.
 *
 * Syscall ABI:
 *   R1        = syscall number (input), return value rval[0] (output)
 *   R2–R4     = arguments 1–3 (in registers)
 *   R2        = rval[1] on success (used by fork/pipe to return 2nd value)
 *   args 4–7  = on user stack at SP+0, SP+4, ... (if needed)
 *   C flag    = 0 on success, 1 on error (R1 = errno)
 *
 * The carry-flag convention (same as aarch64/arm) lets userland
 * distinguish error returns from valid negative return values
 * without ambiguity.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/cpu.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/syscallvar.h>

#include <machine/frame.h>
#include <machine/psl.h>
#include <machine/userret.h>

#include <uvm/uvm_extern.h>

/*
 * Number of syscall arguments passed in registers (R2–R4).
 * Remaining arguments are fetched from the user stack.
 */
#define NARGREG		3

static void syscall(struct trapframe *);

void
syscall_intern(struct proc *p)
{

	p->p_md.md_syscall = syscall;
}

static void
syscall(struct trapframe *tf)
{
	struct lwp * const l = curlwp;
	struct proc * const p = l->l_proc;
	const struct sysent *callp;
	register_t args[SYS_MAXSYSARGS];
	register_t rval[2];
	int code, error;

	LWP_CACHE_CREDS(l, p);
	curcpu()->ci_data.cpu_nsyscall++;

	/*
	 * Advance past SYSCALL instruction so normal return resumes
	 * at the next instruction.
	 */
	tf->tf_epc += 4;

	/* Syscall number from R1 */
	code = tf->tf_regs[TF_R1];

	/*
	 * Indirect syscalls (SYS_syscall, SYS___syscall) need argument
	 * reshuffling we don't support yet.  Reject loudly.
	 */
	if (code == SYS_syscall || code == SYS___syscall) {
		error = ENOSYS;
		goto bad;
	}

	if (code < 0 || code >= p->p_emul->e_nsysent)
		callp = p->p_emul->e_sysent + p->p_emul->e_nosys;
	else
		callp = p->p_emul->e_sysent + code;

	args[0] = tf->tf_regs[TF_R2];
	args[1] = tf->tf_regs[TF_R3];
	args[2] = tf->tf_regs[TF_R4];

	if (callp->sy_narg > NARGREG) {
		error = copyin((const void *)(uintptr_t)tf->tf_regs[TF_R14],
		    &args[NARGREG],
		    (callp->sy_narg - NARGREG) * sizeof(register_t));
		if (error)
			goto bad;
	}

	error = sy_invoke(callp, l, args, rval, code);

	switch (error)
	{
	case EJUSTRETURN:
		break;

	case ERESTART:
		tf->tf_epc -= 4;
		break;

	case 0:
		tf->tf_regs[TF_R1] = rval[0];
		tf->tf_regs[TF_R2] = rval[1];
		tf->tf_sr &= ~PSL_C;
		break;

	default:
	bad:
		if (p->p_emul->e_errno)
			error = p->p_emul->e_errno[error];
		tf->tf_regs[TF_R1] = error;
		tf->tf_sr |= PSL_C;
		break;
	}

	userret(l);
}

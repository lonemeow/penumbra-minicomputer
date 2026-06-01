/*	$NetBSD$	*/

/*
 * Penumbra trap/exception handling.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/syscall.h>

#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/psl.h>
#include <machine/pcb.h>
#include <machine/pmap.h>
#include <machine/sysreg.h>
#include <machine/userret.h>

#include <uvm/uvm_extern.h>

#ifdef DDB
#include <machine/db_machdep.h>
#include <ddb/db_extern.h>
#endif

/* Exception vector numbers (match hardware vector table at PA 0x00) */
#define EXC_BUSFAULT	0
#define EXC_TIMER	1
#define EXC_TLB_MISS	2
#define EXC_TLB_PROT	3
#define EXC_PRIV	4
#define EXC_SYSCALL	5
#define EXC_BREAK	6
#define EXC_ILLEGAL	7
#define EXC_ALIGN	8
#define EXC_EXT_IRQ	9
#define EXC_ARITH	10

/* Forward declarations */
void	trap(struct trapframe *);
void	timer_tc_tick(void);	/* machdep.c — advance timecounter base */

#define FSTAT_TO_VM_PROT(s)                                \
	((((s) & (1 << FSTAT_R)) ? VM_PROT_READ    : 0)   \
	| (((s) & (1 << FSTAT_W)) ? VM_PROT_WRITE   : 0)  \
	| (((s) & (1 << FSTAT_X)) ? VM_PROT_EXECUTE : 0))

static void
user_trap_signal(int signo, int code, vaddr_t addr, int trap,
    struct trapframe *tf)
{
	ksiginfo_t ksi;

	printf("user signal: pid %d (%s) sig %d code %d "
	    "addr 0x%08x pc 0x%08x trap %d\n",
	    curproc->p_pid, curproc->p_comm,
	    signo, code, (uint32_t)addr, (uint32_t)tf->tf_epc, trap);
	printf("  LR=0x%08x SP=0x%08x R1=0x%08x R2=0x%08x "
	    "R11=0x%08x\n",
	    tf->tf_regs[TF_LR], tf->tf_regs[TF_SP],
	    tf->tf_regs[TF_R1], tf->tf_regs[TF_R2],
	    tf->tf_regs[TF_R11]);

	KSI_INIT_TRAP(&ksi);
	ksi.ksi_signo = signo;
	ksi.ksi_code = code;
	ksi.ksi_addr = (void *)addr;
	ksi.ksi_trap = trap;
	trapsignal(curlwp, &ksi);
}

/*
 * trap: main exception dispatch.
 * Called from locore.S exception stubs with trapframe pointer.
 */
void
trap(struct trapframe *tf)
{
	int type = tf->tf_cause;
	int usermode = USERMODE(tf->tf_sr);

	/*
	 * Hardware masks interrupts on exception entry.  For long-running
	 * paths (syscall body, uvm_fault, signal delivery) we re-enable
	 * them so the statclock can sample kernel time and the work is
	 * preemptible.
	 *
	 * EXC_TIMER and EXC_EXT_IRQ stay masked: Penumbra has no per-
	 * source interrupt mask (SR.I is one bit), and both sources are
	 * level-triggered.  EI mid-handler would re-trap immediately —
	 * on the still-high shared IRQ line, or on the same timer
	 * underflow before TMST_UDF is cleared.  Multi-IPL ports work
	 * around this with splraise(handler_ipl)+EI; we can't.
	 *
	 * Traps arriving with PSL_I already clear stay masked: the
	 * caller is in an splhigh critical section whose correctness
	 * depends on interrupts staying off until splx.
	 */

	if (type != EXC_TIMER && type != EXC_EXT_IRQ && (tf->tf_sr & PSL_I) != 0) {
		__asm __volatile("ei" : : : "memory");
	}

	switch (type) {
	case EXC_TIMER: {
		/* Clear timer underflow flag (write-1-to-clear) */
		uint32_t one = TMST_UDF;
		__asm __volatile(
		    "WRSYS %0, %1, %2"
		    : : "r"(one), "i"(SYSDEV_TIMER), "i"(TM_STATUS)
		);

		/* Advance timecounter base before hardclock reads it */
		timer_tc_tick();

		struct clockframe cf;
		cf.cf_pc = tf->tf_epc;
		cf.cf_sr = tf->tf_sr;
		cf.cf_intr_depth = curcpu()->ci_idepth;
		curcpu()->ci_idepth++;
		hardclock(&cf);
		curcpu()->ci_idepth--;
		break;
	}

	case EXC_EXT_IRQ:
		/*
		 * Shared IRQ line: no interrupt controller.
		 * intr_dispatch() walks every registered handler and
		 * each device identifies itself via its status register.
		 */
		curcpu()->ci_idepth++;
		intr_dispatch();
		curcpu()->ci_idepth--;
		break;

	case EXC_TLB_MISS:
	case EXC_TLB_PROT: {
		struct pcb *pcb = lwp_getpcb(curlwp);
		vaddr_t va = trunc_page(tf->tf_badvaddr);
		struct vmspace *vs = curlwp->l_proc->p_vmspace;
		struct vm_map *map;
		vm_prot_t ftype;
		void *onfault;
		int rv;

		/*
		 * Pick the right map: kernel addresses use kernel_map,
		 * user addresses use the process's vm_map.
		 * User mode accessing kernel VA is always illegal.
		 */
		if (usermode && va >= VM_MIN_KERNEL_ADDRESS) {
			user_trap_signal(SIGSEGV, SEGV_MAPERR,
			    tf->tf_badvaddr, type, tf);
			break;
		}
		if (va >= VM_MIN_KERNEL_ADDRESS) {
			/*
			 * Kernel-VA miss: the fast walker resolves whenever
			 * the kernel L1/L2 have a valid PTE for this VA, so
			 * reaching trap here means either a real fault or a
			 * demand-paged kernel mapping that UVM will fill in.
			 * No lazy L1 propagation is needed — the split walker
			 * reads PTEs straight out of the kernel L1 via slot 1.
			 */
			map = kernel_map;
		} else {
			map = &vs->vm_map;
		}

		/*
		 * Determine ftype from hardware fault status.
		 * MMU_FAULT_STATUS bits [10:8] encode the access type:
		 *   bit 8  (FSTAT_R) = read
		 *   bit 9  (FSTAT_W) = write
		 *   bit 10 (FSTAT_X) = execute
		 */
		ftype = FSTAT_TO_VM_PROT(tf->tf_fault_status);

		onfault = pcb->pcb_onfault;
		pcb->pcb_onfault = NULL;
		rv = uvm_fault(map, va, ftype);
		pcb->pcb_onfault = onfault;

		if (rv == 0)
			break;	/* success — RTE retries faulting insn */

		/* uvm_fault failed */
		if (!usermode && onfault != NULL) {
			/* copyin/copyout fault recovery */
			tf->tf_epc = (uint32_t)onfault;
			pcb->pcb_onfault = NULL;
			break;
		}
		if (usermode) {
			user_trap_signal(SIGSEGV,
			    type == EXC_TLB_PROT ? SEGV_ACCERR : SEGV_MAPERR,
			    tf->tf_badvaddr, type, tf);
			break;
		}
#ifdef DDB
		if (db_recover != NULL)
			longjmp(db_recover);
#endif
		panic("kernel %s fault at va=0x%08x, pc=0x%08x "
		    "(rv=%d, fstat=0x%x, ftype=0x%x)",
		    type == EXC_TLB_MISS ? "TLB miss" : "TLB prot",
		    tf->tf_badvaddr, tf->tf_epc, rv,
		    tf->tf_fault_status, ftype);
		break;
	}

	case EXC_BUSFAULT:
		if (usermode) {
			user_trap_signal(SIGBUS, BUS_ADRERR,
			    tf->tf_badvaddr, type, tf);
			break;
		}
#ifdef DDB
		if (db_recover != NULL)
			longjmp(db_recover);
#endif
		panic("kernel bus fault at va=0x%08x, pc=0x%08x",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	case EXC_PRIV:
		if (usermode) {
			user_trap_signal(SIGILL, ILL_PRVOPC,
			    tf->tf_epc, type, tf);
			break;
		}
		panic("kernel privilege violation at pc=0x%08x",
		    tf->tf_epc);
		break;

	case EXC_SYSCALL:
		if (!usermode)
			panic("kernel syscall at pc=0x%08x", tf->tf_epc);
		(*curlwp->l_proc->p_md.md_syscall)(tf);
		break;

	case EXC_BREAK:
		if (usermode) {
			user_trap_signal(SIGTRAP, TRAP_BRKPT,
			    tf->tf_epc, type, tf);
			break;
		}
#ifdef DDB
		if (kdb_trap(type, tf))
			break;
#endif
		panic("BREAK at pc=0x%08x (sr=0x%08x)",
		    tf->tf_epc, tf->tf_sr);
		break;

	case EXC_ILLEGAL:
		if (usermode) {
			user_trap_signal(SIGILL, ILL_ILLOPC,
			    tf->tf_epc, type, tf);
			break;
		}
		panic("kernel illegal instruction at pc=0x%08x",
		    tf->tf_epc);
		break;

	case EXC_ALIGN:
		if (usermode) {
			user_trap_signal(SIGBUS, BUS_ADRALN,
			    tf->tf_badvaddr, type, tf);
			break;
		}
		panic("kernel alignment fault at va=0x%08x, pc=0x%08x",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	case EXC_ARITH:
		if (usermode) {
			user_trap_signal(SIGFPE, FPE_INTDIV,
				tf->tf_epc, type, tf);
			break;
		}
		panic("kernel divide by zero at pc=0x%08x",
		    tf->tf_epc);
		break;

	default:
		panic("unexpected exception type %d at pc=0x%08x",
		    type, tf->tf_epc);
	}

	/*
	 * On return to userland: check for ASTs, pending signals,
	 * and RAS (Restartable Atomic Sequence) restart.
	 */
	if (usermode)
		userret(curlwp, tf);

	/*
	 * The locore trap epilogue stashes ESR/EPC into pinned scratch
	 * before WRSPR; a nested exception would clobber those slots
	 * and corrupt the return.  Mask interrupts now so the epilogue
	 * runs atomically.  ERET restores the saved SR (including I),
	 * so userland comes back up with interrupts enabled.
	 */
	__asm __volatile("DI" ::: "memory");
}

/*
 * Interrupt priority level (SPL) implementation.
 *
 * Uniprocessor with a single timer interrupt: any IPL above NONE
 * disables the CPU interrupt bit (DI), IPL_NONE enables it (EI).
 *
 * The "current IPL" is derived from the hardware SR.I bit, not
 * tracked in a global variable.  This avoids a desync where
 * exception entry clears SR.I (hardware) without updating any
 * software variable — a global would give splraise() stale data,
 * causing splx() to erroneously re-enable interrupts inside
 * exception handlers.
 *
 * For our binary model: SR.I=1 → IPL_NONE, SR.I=0 → IPL_HIGH.
 * NetBSD's multi-level IPL distinctions (SOFTCLOCK, VM, SCHED, …)
 * collapse into this single bit.
 */

static inline int
current_ipl(void)
{
	uint32_t sr;

	__asm __volatile("RDSPR %0, sr" : "=r"(sr));
	return (sr & PSL_I) ? IPL_NONE : IPL_HIGH;
}

int
splraise(int ipl)
{
	int old = current_ipl();

	if (ipl > IPL_NONE)
		__asm __volatile("DI");
	return old;
}

void
splx(int ipl)
{

	if (ipl == IPL_NONE)
		__asm __volatile("EI");
	else
		__asm __volatile("DI");
}

void
spl0(void)
{

	__asm __volatile("EI");
}

int splhigh(void)	{ return splraise(IPL_HIGH); }
int splsoftclock(void)	{ return splraise(IPL_SOFTCLOCK); }
int splsoftbio(void)	{ return splraise(IPL_SOFTBIO); }
int splsoftnet(void)	{ return splraise(IPL_SOFTNET); }
int splsoftserial(void)	{ return splraise(IPL_SOFTSERIAL); }
int splvm(void)		{ return splraise(IPL_VM); }
int splsched(void)	{ return splraise(IPL_SCHED); }
int splddb(void)	{ return splraise(IPL_DDB); }

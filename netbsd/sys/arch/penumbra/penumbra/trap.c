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

/* Forward declarations */
void	trap(struct trapframe *);
void	timer_tc_tick(void);	/* machdep.c — advance timecounter base */

#define FSTAT_TO_VM_PROT(s)                                \
	((((s) & (1 << FSTAT_R)) ? VM_PROT_READ    : 0)   \
	| (((s) & (1 << FSTAT_W)) ? VM_PROT_WRITE   : 0)  \
	| (((s) & (1 << FSTAT_X)) ? VM_PROT_EXECUTE : 0))

static void
user_trap_signal(int signo, int code, vaddr_t addr, int trap)
{
	ksiginfo_t ksi;

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
		/* TODO: read interrupt controller, dispatch by source */
		panic("external IRQ (no handler yet), pc=0x%08x", tf->tf_epc);
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
			    tf->tf_badvaddr, type);
			break;
		}
		if (va >= VM_MIN_KERNEL_ADDRESS)
			map = kernel_map;
		else
			map = &vs->vm_map;

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
			    tf->tf_badvaddr, type);
			break;
		}
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
			    tf->tf_badvaddr, type);
			break;
		}
		panic("kernel bus fault at va=0x%08x, pc=0x%08x",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	case EXC_PRIV:
		if (usermode) {
			user_trap_signal(SIGILL, ILL_PRVOPC,
			    tf->tf_epc, type);
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
			    tf->tf_epc, type);
			break;
		}
		/* Kernel BREAK — halt (future: DDB entry point) */
		panic("BREAK at pc=0x%08x (sr=0x%08x)",
		    tf->tf_epc, tf->tf_sr);
		break;

	case EXC_ILLEGAL:
		if (usermode) {
			user_trap_signal(SIGILL, ILL_ILLOPC,
			    tf->tf_epc, type);
			break;
		}
		panic("kernel illegal instruction at pc=0x%08x",
		    tf->tf_epc);
		break;

	case EXC_ALIGN:
		if (usermode) {
			user_trap_signal(SIGBUS, BUS_ADRALN,
			    tf->tf_badvaddr, type);
			break;
		}
		panic("kernel alignment fault at va=0x%08x, pc=0x%08x",
		    tf->tf_badvaddr, tf->tf_epc);
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
}

/*
 * Interrupt priority level (SPL) implementation.
 *
 * Uniprocessor with a single timer interrupt: any IPL above NONE
 * disables the CPU interrupt bit (DI), IPL_NONE enables it (EI).
 * We track the current IPL so splx() can restore the previous state.
 */
static int current_ipl;

void
intr_init(void)
{
	current_ipl = IPL_HIGH;		/* interrupts off at boot */
}

int
splraise(int ipl)
{
	int old = current_ipl;

	if (ipl > current_ipl) {
		current_ipl = ipl;
		__asm __volatile("DI");
	}
	return old;
}

void
splx(int ipl)
{
	current_ipl = ipl;
	if (ipl == IPL_NONE)
		__asm __volatile("EI");
	else
		__asm __volatile("DI");
}

void
spl0(void)
{
	current_ipl = IPL_NONE;
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

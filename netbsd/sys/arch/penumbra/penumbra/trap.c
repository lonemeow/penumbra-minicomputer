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

#include <uvm/uvm_extern.h>

/* Exception vector numbers (match hardware vector table at PA 0x00) */
#define EXC_BUSFAULT	0
#define EXC_IRQ		1
#define EXC_TLB_MISS	2
#define EXC_TLB_PROT	3
#define EXC_PRIV	4
#define EXC_SYSCALL	5
#define EXC_BREAK	6
#define EXC_ILLEGAL	7
#define EXC_ALIGN	8

/* Forward declaration */
void	trap(struct trapframe *);

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
	case EXC_IRQ:
		/* TODO: call interrupt dispatcher */
		panic("IRQ (no handler yet), pc=0x%08x", tf->tf_epc);
		break;

	case EXC_TLB_MISS:
	case EXC_TLB_PROT:
		/* TODO: call uvm_fault() for demand paging */
		panic("%s %s fault at va=0x%08x, pc=0x%08x",
		    usermode ? "user" : "kernel",
		    type == EXC_TLB_MISS ? "TLB miss" : "TLB prot",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	case EXC_BUSFAULT:
		panic("%s bus fault at va=0x%08x, pc=0x%08x",
		    usermode ? "user" : "kernel",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	case EXC_PRIV:
		panic("%s privilege violation at pc=0x%08x",
		    usermode ? "user" : "kernel", tf->tf_epc);
		break;

	case EXC_SYSCALL:
		/* TODO: system call dispatch */
		panic("syscall from %s at pc=0x%08x",
		    usermode ? "user" : "kernel", tf->tf_epc);
		break;

	case EXC_BREAK:
		/* TODO: DDB entry point, or deliver SIGTRAP */
		panic("BREAK at pc=0x%08x (sr=0x%08x)",
		    tf->tf_epc, tf->tf_sr);
		break;

	case EXC_ILLEGAL:
		panic("%s illegal instruction at pc=0x%08x",
		    usermode ? "user" : "kernel", tf->tf_epc);
		break;

	case EXC_ALIGN:
		panic("%s alignment fault at va=0x%08x, pc=0x%08x",
		    usermode ? "user" : "kernel",
		    tf->tf_badvaddr, tf->tf_epc);
		break;

	default:
		panic("unexpected exception type %d at pc=0x%08x",
		    type, tf->tf_epc);
	}
}

/*
 * Interrupt SPL stubs — these will be real once the
 * interrupt controller is implemented.
 */
void
intr_init(void)
{
	/* TODO: set up interrupt controller */
}

int
splraise(int ipl)
{
	/* TODO: disable interrupts, return old IPL */
	return 0;
}

void
splx(int ipl)
{
	/* TODO: restore IPL */
}

void
spl0(void)
{
	/* TODO: enable all interrupts */
}

int splhigh(void)	{ return splraise(IPL_HIGH); }
int splsoftclock(void)	{ return splraise(IPL_SOFTCLOCK); }
int splsoftbio(void)	{ return splraise(IPL_SOFTBIO); }
int splsoftnet(void)	{ return splraise(IPL_SOFTNET); }
int splsoftserial(void)	{ return splraise(IPL_SOFTSERIAL); }
int splvm(void)		{ return splraise(IPL_VM); }
int splsched(void)	{ return splraise(IPL_SCHED); }
int splddb(void)	{ return splraise(IPL_DDB); }

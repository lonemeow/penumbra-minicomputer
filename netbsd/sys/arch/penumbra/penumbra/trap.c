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

/* Exception vector numbers (match hardware dispatch) */
#define EXC_RESET	0
#define EXC_IRQ		1
#define EXC_TLB_MISS	2
#define EXC_TLB_PROT	3
#define EXC_BUSFAULT	4
#define EXC_BREAK	5
#define EXC_SYSCALL	6
#define EXC_ILLEGAL	7

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
		break;

	case EXC_TLB_MISS:
	case EXC_TLB_PROT:
		/*
		 * TLB miss/protection: call pmap to load TLB entry.
		 * If pmap can't handle it, deliver SIGSEGV to process
		 * (user) or panic (kernel).
		 */
		/* TODO: pmap_tlb_miss(tf->tf_badvaddr, type, tf) */
		if (usermode) {
			printf("user TLB fault at 0x%08x, pc=0x%08x\n",
			    tf->tf_badvaddr, tf->tf_epc);
		} else {
			panic("kernel TLB fault at 0x%08x, pc=0x%08x",
			    tf->tf_badvaddr, tf->tf_epc);
		}
		break;

	case EXC_BUSFAULT:
		if (usermode) {
			printf("user bus fault at pc=0x%08x\n", tf->tf_epc);
		} else {
			panic("kernel bus fault at pc=0x%08x", tf->tf_epc);
		}
		break;

	case EXC_BREAK:
		/* TODO: DDB entry point, or deliver SIGTRAP */
		printf("BREAK at pc=0x%08x\n", tf->tf_epc);
		break;

	case EXC_SYSCALL:
		/* TODO: system call dispatch */
		if (usermode) {
			printf("syscall from pc=0x%08x\n", tf->tf_epc);
			/* Advance PC past SYSCALL instruction */
			tf->tf_epc += 4;
		} else {
			panic("syscall in kernel mode at pc=0x%08x",
			    tf->tf_epc);
		}
		break;

	case EXC_ILLEGAL:
		if (usermode) {
			printf("illegal instruction at pc=0x%08x\n",
			    tf->tf_epc);
		} else {
			panic("illegal instruction in kernel at pc=0x%08x",
			    tf->tf_epc);
		}
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

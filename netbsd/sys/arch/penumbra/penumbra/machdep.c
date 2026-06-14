/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent kernel runtime.
 *
 * cpu_startup, cpu_reboot, LWP/process management stubs,
 * signal support, user memory access, and other MD hooks
 * required by the MI kernel.
 *
 * Early boot initialization is in startup.c.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/device.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/buf.h>
#include <sys/core.h>
#include <sys/syscallargs.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/ptrace.h>
#include <sys/timetc.h>
#include <sys/endian.h>

#include <uvm/uvm_extern.h>

#include <machine/bootinfo.h>
#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/pcb.h>
#include <machine/frame.h>
#include <machine/vmparam.h>
#include <machine/pmap.h>
#include <machine/reg.h>
#include <machine/sysreg.h>
#include <machine/mcontext.h>
#include <machine/userret.h>

#include "ksyms.h"

#if NKSYMS || defined(DDB) || defined(MODULAR)
#include <sys/ksyms.h>
#endif

/* Single CPU info structure (uniprocessor) */
struct cpu_info cpu_info_store = {
	.ci_curlwp = &lwp0,
};

/* Physical memory regions */
struct vm_map *phys_map;

/* Machine name strings (referenced by MI kernel) */
char machine[] = "penumbra";
char machine_arch[] = "penumbra";

/*
 * cpu_startup: called from init_main.c after basic VM is running.
 * Prints startup banner, allocates submap for physio.
 *
 * The machine identity is read from SYSDEV_MACH (board-specific:
 * different RTL on different boards, or sim vs. ULX3S, produces a
 * different name).  CPU identity is reported separately by cpu_attach
 * in the autoconf "cpu0 at mainbus0:" line.
 */
void
cpu_startup(void)
{
	char mach_name[17];
	char pbuf[9];	/* "99999 MB" */
	uint32_t w0, w1, w2, w3;

	printf("%s%s", copyright, version);

	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w0) : "i"(SYSDEV_MACH), "i"(MACH_NAME0));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w1) : "i"(SYSDEV_MACH), "i"(MACH_NAME1));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w2) : "i"(SYSDEV_MACH), "i"(MACH_NAME2));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w3) : "i"(SYSDEV_MACH), "i"(MACH_NAME3));
	le32enc(mach_name +  0, w0);
	le32enc(mach_name +  4, w1);
	le32enc(mach_name +  8, w2);
	le32enc(mach_name + 12, w3);
	mach_name[16] = '\0';

	if (mach_name[0] != '\0')
		cpu_setmodel("Penumbra %s", mach_name);
	else
		cpu_setmodel("Penumbra (unknown)");

	printf("%s\n", cpu_getmodel());

	format_bytes(pbuf, sizeof(pbuf), ctob(physmem));
	printf("total memory = %s\n", pbuf);

	format_bytes(pbuf, sizeof(pbuf), ptoa(uvm_availmem(false)));
	printf("avail memory = %s\n", pbuf);

#if NKSYMS || defined(DDB) || defined(MODULAR)
	{
		struct btinfo_symtab *bi_sym;

		/*
		 * Install PTEs for the post-_end region where the bootloader
		 * placed the symbol table.  Deferred from pmap_bootstrap()
		 * to avoid trashing the early UART's pinned scratch slot;
		 * by the time cpu_startup runs, the UART is on its permanent VA.
		 */
		pmap_map_kernel_tail();

		bi_sym = lookup_bootinfo(BTINFO_SYMTAB);
		if (bi_sym != NULL && bi_sym->ssym != 0 && bi_sym->esym != 0) {
			ksyms_addsyms_elf(bi_sym->esym - bi_sym->ssym,
			    (void *)(uintptr_t)bi_sym->ssym,
			    (void *)(uintptr_t)bi_sym->esym);
		}
	}
#endif

	/* TODO: allocate physio submap via uvm_km_suballoc */
}

/*
 * Machine-dependent reboot.
 */
void
cpu_reboot(int howto, char *bootstr)
{

	/* Disable interrupts */
	splhigh();

	if (howto & RB_HALT) {
		printf("halted.\n\n");
	} else {
		printf("rebooting...\n\n");
	}

	/* TODO: actual reset — write to a reset sysreg */
	__asm volatile("break");
	for (;;)
		;
}

/*
 * Timecounter — synthesises a monotonic 32-bit counter from the
 * hardware timer's 16-bit countdown register.
 *
 * The MI timecounter API requires the counter to wrap at a power-of-2
 * matching tc_counter_mask.  Our hardware timer wraps at (reload+1)
 * which is 10000 for 100 Hz — not a power of 2.
 *
 * Solution: accumulate a base counter in the hardclock handler
 * (bumped by reload+1 each interrupt), and add the current sub-tick
 * offset.  The result is a continuous 32-bit value at 1 MHz, wrapping
 * naturally at 2^32 (~71 minutes).
 *
 * Monotonicity: hardware autoloads `count` and latches TM_STATUS.UDF
 * at the underflow cycle, but `timer_tc_base` is only bumped when
 * the timer ISR actually runs.  Between hardware underflow and ISR
 * entry — a window widened by any kernel critical section that
 * masks interrupts — a naive `base + (reload - count)` would return
 * a value strictly less than the previous reading, violating the
 * POSIX CLOCK_MONOTONIC contract.
 *
 * Defence: read TM_STATUS in addition to count, so the reader can
 * detect an unprocessed wrap and advance base by one period.  Sample
 * timer_tc_base and TM_STATUS on both sides of the count read, retry
 * if either changed.  On a uniprocessor the ISR preempts the reader
 * and runs to completion atomically, so the only observable stable
 * states are pre-ISR (UDF=set, base=N) and post-ISR (UDF=clear,
 * base=N+period); a snapshot that straddles the ISR shows up as
 * either base_a != base_b or status_a != status_b and is retried.
 */
static uint32_t timer_reload_val;
static volatile uint32_t timer_tc_base;	/* accumulated ticks at last interrupt */

void	timer_tc_tick(void);	/* called from trap.c timer handler */
void	timer_early_init(void);	/* called from startup.c before autoconf */

static u_int
timer_get_timecount(struct timecounter *tc)
{
	uint32_t base_a, base_b, count, status_a, status_b;

	for (;;) {
		base_a = timer_tc_base;
		__asm __volatile(
		    "RDSYS %0, %1, %2" : "=r"(status_a)
		    : "i"(SYSDEV_TIMER), "i"(TM_STATUS)
		);
		__asm __volatile(
		    "RDSYS %0, %1, %2" : "=r"(count)
		    : "i"(SYSDEV_TIMER), "i"(TM_COUNT)
		);
		__asm __volatile(
		    "RDSYS %0, %1, %2" : "=r"(status_b)
		    : "i"(SYSDEV_TIMER), "i"(TM_STATUS)
		);
		base_b = timer_tc_base;

		if (base_a == base_b && status_a == status_b)
			break;
	}

	/*
	 * UDF set in the stable snapshot means hardware has wrapped
	 * past a period boundary that the ISR hasn't yet processed.
	 * `count` is from the new period; compensate by advancing
	 * base by one period.
	 */
	if (status_a & TMST_UDF)
		base_a += timer_reload_val + 1;
	return base_a + (timer_reload_val - count);
}

/*
 * Called from the timer interrupt handler (trap.c) to advance
 * the timecounter base by one period.
 */
void
timer_tc_tick(void)
{
	timer_tc_base += timer_reload_val + 1;
}

static struct timecounter timer_timecounter = {
	.tc_get_timecount	= timer_get_timecount,
	.tc_counter_mask	= ~0u,
	.tc_frequency		= 0,		/* set at init */
	.tc_name		= "penumbra_timer",
	.tc_quality		= 100,
};

/*
 * Clock initialization — program the hardware timer for periodic
 * interrupts at hz (default 100) ticks per second, and register
 * the timecounter for sub-tick timestamp resolution.
 */
void
cpu_initclocks(void)
{
	uint32_t freq;
	uint32_t reload;

	/* Read timer tick frequency from hardware */
	__asm __volatile(
	    "RDSYS %0, %1, %2"
	    : "=r"(freq) : "i"(SYSDEV_TIMER), "i"(TM_FREQ)
	);

	/* Compute reload value: period = (reload + 1) ticks */
	reload = freq / hz - 1;

	printf("timer: %u Hz tick, reload %u for %d Hz hardclock\n",
	    freq, reload, hz);

	/* Set reload and initial count */
	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(reload), "i"(SYSDEV_TIMER), "i"(TM_RELOAD)
	);
	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(reload), "i"(SYSDEV_TIMER), "i"(TM_COUNT)
	);

	/* Enable: TICK_EN | IRQ_EN | AUTOLOAD */
	uint32_t cr = TMCR_TICK_EN | TMCR_IRQ_EN | TMCR_AUTOLOAD;
	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(cr), "i"(SYSDEV_TIMER), "i"(TM_CR)
	);

	/* Register timecounter for sub-tick timestamp resolution */
	timer_reload_val = reload;
	timer_timecounter.tc_frequency = freq;
	tc_init(&timer_timecounter);
}

void
setstatclockrate(int rate)
{
	/* no stat clock yet */
}

/*
 * Start the timer as a free-running counter for delay().
 *
 * Called early from penumbra_init(), before autoconf.  Sets up
 * the timer with max reload and TICK_EN + AUTOLOAD but no IRQ,
 * so delay() has a working 1 MHz counter from the start.
 * cpu_initclocks() later reconfigures for periodic interrupts.
 */
void
timer_early_init(void)
{
	uint32_t reload = 0xFFFF;	/* max 16-bit countdown */
	uint32_t cr = TMCR_TICK_EN | TMCR_AUTOLOAD;

	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(reload), "i"(SYSDEV_TIMER), "i"(TM_RELOAD)
	);
	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(reload), "i"(SYSDEV_TIMER), "i"(TM_COUNT)
	);
	__asm __volatile(
	    "WRSYS %0, %1, %2"
	    : : "r"(cr), "i"(SYSDEV_TIMER), "i"(TM_CR)
	);
}

/*
 * Microsecond delay — busy-wait using the hardware timer counter.
 *
 * The timer ticks at 1 MHz (1 tick = 1 µs), counting down with
 * auto-reload.  We accumulate elapsed ticks by reading TMCOUNT
 * snapshots, handling the reload wrap.
 *
 * timer_early_init() ensures the counter is running before
 * autoconf, so delay() works from the very first driver probe.
 */
void
delay(unsigned int us)
{
	uint32_t prev, now, elapsed = 0;

	__asm __volatile(
	    "RDSYS %0, %1, %2" : "=r"(prev)
	    : "i"(SYSDEV_TIMER), "i"(TM_COUNT)
	);

	while (elapsed < us) {
		__asm __volatile(
		    "RDSYS %0, %1, %2" : "=r"(now)
		    : "i"(SYSDEV_TIMER), "i"(TM_COUNT)
		);
		if (now <= prev)
			elapsed += prev - now;
		else
			elapsed += prev + 1;	/* wrapped past zero */
		prev = now;
	}
}

/*
 * Idle loop.
 */
void
cpu_idle(void)
{

	/*
	 * Ensure interrupts are enabled while waiting.
	 *
	 * The MI idle loop (kern_idle.c) calls spl0() before the
	 * main loop, but mutex operations inside sched_idle() and
	 * uvm_idle() may leave the hardware interrupt bit cleared
	 * due to the binary nature of our SPL model (any IPL > NONE
	 * disables interrupts; nested mutex release can't distinguish
	 * "restore to NONE" from "restore to SOFTCLOCK").
	 *
	 * Other ports with similar constraints (ARM wfi, MIPS wait)
	 * ensure interrupts are enabled in cpu_idle for the same
	 * reason — the CPU must be interruptible to receive timer
	 * ticks and process callouts.
	 */
	spl0();
}

/*
 * cpu_dumpconf — configure crash dump device.
 */
void
cpu_dumpconf(void)
{
	/* no dump support yet */
}

/*
 * cpu_intr_p — return true if running in interrupt context.
 */
bool
cpu_intr_p(void)
{

	return curcpu()->ci_idepth > 0;
}

/*
 * Machine-dependent LWP operations — stubs.
 */
void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int flags)
{
	/* UP only: SMP would need IPI when ci != curcpu(). */
	ci->ci_want_resched = 1;
}

void
cpu_need_proftick(struct lwp *l)
{
	/* no profiling yet */
}

void
cpu_signotify(struct lwp *l)
{
	/* UP only: SMP would need IPI when l->l_cpu != curcpu(). */
	l->l_md.md_astpending = 1;
}

void
cpu_lwp_free(struct lwp *l, int proc)
{
	/* nothing */
}

void
cpu_lwp_free2(struct lwp *l)
{
	/* nothing */
}

int
cpu_lwp_setprivate(struct lwp *l, void *v)
{

	l->l_md.md_utf->tf_regs[TF_TP] = (uint32_t)(uintptr_t)v;
	return 0;
}

/*
 * Assembly trampoline for new LWPs (in locore.S).
 * cpu_switchto restores pcb_context with LR = lwp_trampoline,
 * which calls func(arg) then returns to userspace.
 */
extern void lwp_trampoline(void);

/*
 * cpu_lwp_fork — set up a new LWP so cpu_switchto can resume it.
 *
 * Creates the child's kernel stack layout:
 *
 *   uarea base (+0)       → struct pcb (includes pcb_context)
 *                           kernel stack space (grows up)
 *   uarea + USPACE - TF   → struct trapframe (copy of parent's)
 *   uarea + USPACE
 *
 * pcb_context is set so that when cpu_switchto does its longjmp-style
 * restore, the child resumes in lwp_trampoline which calls func(arg).
 *
 * label_t layout: [0]=R5 [4]=R6 [8]=R7 [12]=R8 [16]=R9 [20]=R10
 *                 [24]=R12(TP) [28]=R13(LR) [32]=R14(SP)
 */
void
cpu_lwp_fork(struct lwp *l1, struct lwp *l2, void *stack, size_t stacksize,
    void (*func)(void *), void *arg)
{
	struct pcb *pcb1 = lwp_getpcb(l1);
	struct pcb *pcb2 = lwp_getpcb(l2);

	/* Start with a copy of the parent's PCB (onfault, etc.) */
	*pcb2 = *pcb1;
	pcb2->pcb_onfault = NULL;

	/* Child's uarea base (set by the MI layer before calling us) */
	vaddr_t uarea = uvm_lwp_getuarea(l2);

	/* Place trapframe at the top of the uarea (same layout as lwp0) */
	struct trapframe *tf2 = (struct trapframe *)(uarea + USPACE) - 1;

	/* Copy parent's trapframe to child */
	struct trapframe *tf1 = l1->l_md.md_utf;
	*tf2 = *tf1;

	/* Child returns 0 from fork (R1 = return value) */
	tf2->tf_regs[TF_R1] = 0;

	/* If caller provided a user stack, update the child's SP */
	if (stack != NULL)
		tf2->tf_regs[TF_R14] = (uint32_t)((char *)stack + stacksize);

	l2->l_md.md_utf = tf2;
	l2->l_md.md_astpending = 0;

	/*
	 * Set up pcb_context so cpu_switchto resumes into lwp_trampoline.
	 * lwp_trampoline expects: R5=func, R6=arg, R7=newlwp.
	 *
	 * R12 (curlwp) must be seeded explicitly: the *pcb2 = *pcb1 copy above
	 * left the parent's R12 in the child's context, and cpu_switchto
	 * restores R12 from pcb_context, so without this the child would resume
	 * with the parent as curlwp.
	 */
	pcb2->pcb_context.val[_JB_R5]  = (register_t)func;
	pcb2->pcb_context.val[_JB_R6]  = (register_t)arg;
	pcb2->pcb_context.val[_JB_R7]  = (register_t)l2;
	pcb2->pcb_context.val[_JB_R12] = (register_t)l2;	/* curlwp */
	pcb2->pcb_context.val[_JB_R13] = (register_t)lwp_trampoline;
	pcb2->pcb_context.val[_JB_R14] = (register_t)tf2;
}

/*
 * startlwp — trampoline for new user LWPs (fork/clone).
 *
 * Called from lwp_trampoline as func(arg) where arg is a ucontext_t*.
 * Applies the saved user context and returns to userspace.
 */
void
startlwp(void *arg)
{
	ucontext_t * const uc = arg;
	struct lwp * const l = curlwp;
	int error __diagused;

	error = cpu_setmcontext(l, &uc->uc_mcontext, uc->uc_flags);
	KASSERT(error == 0);

	kmem_free(uc, sizeof(ucontext_t));
	userret(l, l->l_md.md_utf);
}

void
md_child_return(struct lwp *l)
{
	struct trapframe *tf = l->l_md.md_utf;

	/*
	 * Set up the child's return values for fork(2):
	 *   R1 = 0     (fork returns 0 in child)
	 *   R2 = 1     (rval[1] = 1 marks this as the child)
	 *   C flag clear (success)
	 */
	tf->tf_regs[TF_R1] = 0;
	tf->tf_regs[TF_R2] = 1;
	tf->tf_sr &= ~PSL_C;
}

/*
 * cpu_spawn_return: return to userland after execve/fork.
 *
 * The MI layer calls this to jump to user mode.  The trapframe
 * was set up by setregs (exec) or cpu_lwp_fork (fork).
 * Nothing MD-specific needed here — the normal trap return in
 * locore.S restores the trapframe and does RTE.
 */
void
cpu_spawn_return(struct lwp *l)
{
	/* Nothing to do — trap return handles it */
}

/*
 * setregs: set up initial user registers for exec.
 *
 * Initialize the trapframe so that when we return to user mode,
 * the process starts executing at pack->ep_entry with the stack
 * at 'stack'.  Clear all GPRs to avoid leaking kernel state.
 */
void
setregs(struct lwp *l, struct exec_package *pack, vaddr_t stack)
{
	struct trapframe *tf = l->l_md.md_utf;
	struct proc *p = l->l_proc;

	/* Clear all registers — don't leak kernel state */
	memset(tf, 0, sizeof(*tf));

	/* Entry point and stack pointer */
	tf->tf_epc = pack->ep_entry;
	tf->tf_regs[TF_R14] = stack;	/* SP */

	/*
	 * Set up arguments for _start -> ___start(cleanup, ps_strings):
	 *   R1 = cleanup (0 for static binaries, set by rtld for dynamic)
	 *   R2 = ps_strings pointer
	 */
	tf->tf_regs[TF_R1] = 0;		/* cleanup */
	tf->tf_regs[TF_R2] = p->p_psstrp;	/* ps_strings */

	/* User mode: supervisor off, interrupts on */
	tf->tf_sr = PSL_USERSET & ~PSL_USERCLR;
}

/* syscall_intern() is now in syscall.c */

/*
 * Signal and debug support — stubs.
 */
void
cpu_getmcontext(struct lwp *l, mcontext_t *mcp, unsigned int *flags)
{
	const struct trapframe *tf = l->l_md.md_utf;

	/* Copy R0–R15 and SR into the gregset */
	memcpy(mcp->__gregs, tf->tf_regs, sizeof(tf->tf_regs));
	mcp->__gregs[_REG_PC] = tf->tf_epc;	/* EPC is the real saved PC */
	mcp->__gregs[_REG_SR] = tf->tf_sr;

	*flags |= _UC_CPU | _UC_TLSBASE;
}

int
cpu_setmcontext(struct lwp *l, const mcontext_t *mcp, unsigned int flags)
{
	struct trapframe *tf = l->l_md.md_utf;

	if (flags & _UC_CPU) {
		int error = cpu_mcontext_validate(l, mcp);
		if (error)
			return error;

		/* Restore R0–R14 from the gregset */
		memcpy(tf->tf_regs, mcp->__gregs, 15 * sizeof(__greg_t));

		/* Restore PC from the mcontext (slot 15 → EPC) */
		tf->tf_epc = mcp->__gregs[_REG_PC];

		/*
		 * Restore only user-settable SR bits (condition flags).
		 * Supervisor and interrupt bits are forced to user-mode values.
		 */
		tf->tf_sr = (mcp->__gregs[_REG_SR] & PSL_FLAGS)
		    | (PSL_USERSET & ~PSL_USERCLR);
	}

	if (flags & _UC_TLSBASE) {
		/* Restore thread pointer (R12) */
		tf->tf_regs[TF_R12] = mcp->__gregs[12];
		lwp_setprivate(l, (void *)(uintptr_t)mcp->__gregs[12]);
	}

	return 0;
}

int
cpu_mcontext_validate(struct lwp *l, const mcontext_t *mcp)
{

	/* PC must be in user address space */
	if ((uint32_t)mcp->__gregs[_REG_PC] >= VM_MAXUSER_ADDRESS)
		return EINVAL;

	/* SP must be in user address space */
	if ((uint32_t)mcp->__gregs[_REG_SP] >= VM_MAXUSER_ADDRESS)
		return EINVAL;

	/* Must not set supervisor or clear interrupt-enable */
	if (mcp->__gregs[_REG_SR] & PSL_S)
		return EINVAL;

	return 0;
}

/*
 * Signal frame layout pushed onto the user stack:
 *
 *     ┌──────────────┐  ← old SP (or signal stack top)
 *     │  siginfo_t   │  sf_si
 *     │  ucontext_t  │  sf_uc (contains mcontext with saved regs)
 *     └──────────────┘  ← sf (new SP)
 *
 * The signal handler is called as:
 *     handler(signo, &sf->sf_si, &sf->sf_uc)
 * with LR pointing to the libc sigtramp (__sigtramp_siginfo_2).
 */
struct sigframe_siginfo {
	siginfo_t	sf_si;
	ucontext_t	sf_uc;
};

void
sendsig_siginfo(const ksiginfo_t *ksi, const sigset_t *mask)
{
	struct lwp * const l = curlwp;
	struct proc * const p = l->l_proc;
	struct sigacts * const sa = p->p_sigacts;
	struct trapframe * const tf = l->l_md.md_utf;
	const int signo = ksi->ksi_signo;
	const sig_t catcher = SIGACTION(p, signo).sa_handler;
	bool onstack;
	int error;

	/* Determine where to put the signal frame: signal stack or user stack */
	onstack = (l->l_sigstk.ss_flags & (SS_DISABLE | SS_ONSTACK)) == 0
	    && (SIGACTION(p, signo).sa_flags & SA_ONSTACK) != 0;
	vaddr_t sp;
	if (onstack)
		sp = (vaddr_t)l->l_sigstk.ss_sp + l->l_sigstk.ss_size;
	else
		sp = tf->tf_regs[TF_R14];

	/* Allocate the signal frame below the chosen stack pointer */
	struct sigframe_siginfo *sf =
	    (struct sigframe_siginfo *)sp - 1;

	/* Build the signal frame in kernel memory, then copyout */
	struct sigframe_siginfo ksf;
	memset(&ksf, 0, sizeof(ksf));
	ksf.sf_si._info = ksi->ksi_info;
	ksf.sf_uc.uc_flags = _UC_SIGMASK
	    | (l->l_sigstk.ss_flags & SS_ONSTACK ? _UC_SETSTACK : _UC_CLRSTACK);
	ksf.sf_uc.uc_sigmask = *mask;
	ksf.sf_uc.uc_link = l->l_ctxlink;
	sendsig_reset(l, signo);

	mutex_exit(p->p_lock);
	cpu_getmcontext(l, &ksf.sf_uc.uc_mcontext, &ksf.sf_uc.uc_flags);
	error = copyout(&ksf, sf, sizeof(ksf));
	mutex_enter(p->p_lock);

	if (error != 0) {
		/* Stack trashed — kill the process */
		sigexit(l, SIGILL);
		/* NOTREACHED */
	}

	tf->tf_regs[TF_R1] = signo;
	tf->tf_regs[TF_R2] = (intptr_t)&sf->sf_si;
	tf->tf_regs[TF_R3] = (intptr_t)&sf->sf_uc;
	tf->tf_regs[TF_R5] = (intptr_t)&sf->sf_uc;
	tf->tf_regs[TF_R13] = (intptr_t)sa->sa_sigdesc[signo].sd_tramp;
	tf->tf_regs[TF_R14] = (intptr_t)sf;
	tf->tf_epc = (intptr_t)catcher;

	if (onstack)
		l->l_sigstk.ss_flags |= SS_ONSTACK;
}

int
process_read_regs(struct lwp *l, struct reg *regs)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

int
process_write_regs(struct lwp *l, const struct reg *regs)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

int
process_set_pc(struct lwp *l, void *addr)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

int
cpu_coredump(struct lwp *l, struct coredump_iostate *iocookie,
    struct core *chdr)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

/*
 * Physio buffer mapping — stubs.
 */
int
vmapbuf(struct buf *bp, vsize_t len)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

void
vunmapbuf(struct buf *bp, vsize_t len)
{
	/* TODO(stub) */ __asm volatile("break");
}

/*
 * sysarch syscall — stub.
 */
int
sys_sysarch(struct lwp *l, const struct sys_sysarch_args *uap,
    register_t *retval)
{
	return ENOSYS;
}

/*
 * kcopy is implemented in copy.S — it shares the pcb_onfault
 * fault-recovery pattern with copyin/copyout but additionally
 * saves and restores any pre-existing pcb_onfault, because
 * uiomove() can invoke kcopy from inside an outer onfault
 * context.
 */

/*
 * mm_md_physacc — check physical memory accessibility for /dev/mem.
 * Returns 0 if accessible, error otherwise.
 * Declared in <dev/mm.h>.
 */
#include <dev/mm.h>

int
mm_md_physacc(paddr_t pa, vm_prot_t prot)
{
	/* Allow all physical access for now */
	return 0;
}

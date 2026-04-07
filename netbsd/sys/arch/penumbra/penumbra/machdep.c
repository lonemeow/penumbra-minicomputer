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

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/pcb.h>
#include <machine/frame.h>
#include <machine/vmparam.h>
#include <machine/pmap.h>
#include <machine/reg.h>
#include <machine/mcontext.h>

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
 */
void
cpu_startup(void)
{

	printf("%s%s", copyright, version);
	printf("Penumbra minicomputer, 32-bit RISC\n");

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
 * Clock initialization — stub.
 * TODO: implement with a programmable timer.
 */
void
cpu_initclocks(void)
{
	/* no timer hardware yet */
}

void
setstatclockrate(int rate)
{
	/* no stat clock yet */
}

/*
 * Microsecond delay — busy-loop stub.
 * TODO: calibrate against a timer.
 */
void
delay(unsigned int us)
{
	volatile unsigned int i;
	for (i = 0; i < us * 10; i++)
		;
}

/*
 * Idle loop.
 */
void
cpu_idle(void)
{
	/* TODO: WFI or similar low-power instruction */
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
	/* TODO: track interrupt nesting depth */
	return false;
}

/*
 * Machine-dependent LWP operations — stubs.
 */
void
cpu_need_resched(struct cpu_info *ci, struct lwp *l, int flags)
{
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
	/* TODO */
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
	/* TODO: set thread pointer (R12) in trapframe */
	return 0;
}

/*
 * Process/LWP management — stubs.
 * TODO: implement for real once context switching works.
 */
void
cpu_lwp_fork(struct lwp *l1, struct lwp *l2, void *stack, size_t stacksize,
    void (*func)(void *), void *arg)
{
	/* TODO(stub) */ __asm volatile("break");
}

void
startlwp(void *arg)
{
	/* TODO(stub) */ __asm volatile("break");
}

void
md_child_return(struct lwp *l)
{
	/* TODO(stub) */ __asm volatile("break");
}

void
cpu_spawn_return(struct lwp *l)
{
	/* TODO(stub) */ __asm volatile("break");
}

void
setregs(struct lwp *l, struct exec_package *pack, vaddr_t stack)
{
	/* TODO(stub) */ __asm volatile("break");
}

void
syscall_intern(struct proc *p)
{
	/* TODO: set p->p_md.md_syscall */
}

/*
 * Signal and debug support — stubs.
 */
void
cpu_getmcontext(struct lwp *l, mcontext_t *mcp, unsigned int *flags)
{
	/* TODO(stub) */ __asm volatile("break");
}

int
cpu_setmcontext(struct lwp *l, const mcontext_t *mcp, unsigned int flags)
{
	/* TODO(stub) */ __asm volatile("break");
	return 0;
}

int
cpu_mcontext_validate(struct lwp *l, const mcontext_t *mcp)
{
	/* TODO(stub) */
	return 0;
}

void
sendsig_siginfo(const ksiginfo_t *ksi, const sigset_t *mask)
{
	/* TODO(stub) */ __asm volatile("break");
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
 * kcopy — kernel-to-kernel copy with fault protection.
 */
int
kcopy(const void *src, void *dst, size_t len)
{
	/* TODO(stub): implement with pcb_onfault recovery */
	memcpy(dst, src, len);
	return 0;
}

/*
 * copyoutstr — copy kernel string to user space.
 * TODO: implement with fault recovery.
 */
int
copyoutstr(const void *kaddr, void *uaddr, size_t len, size_t *done)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

/*
 * ufetch/ustore — single-value user memory access.
 * TODO: implement with fault recovery.
 */
int _ufetch_8(const uint8_t *uaddr, uint8_t *valp);
int _ufetch_16(const uint16_t *uaddr, uint16_t *valp);
int _ufetch_32(const uint32_t *uaddr, uint32_t *valp);
int _ustore_8(uint8_t *uaddr, uint8_t val);
int _ustore_16(uint16_t *uaddr, uint16_t val);
int _ustore_32(uint32_t *uaddr, uint32_t val);

int
_ufetch_8(const uint8_t *uaddr, uint8_t *valp)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

int
_ufetch_16(const uint16_t *uaddr, uint16_t *valp)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

int
_ufetch_32(const uint32_t *uaddr, uint32_t *valp)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

int
_ustore_8(uint8_t *uaddr, uint8_t val)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

int
_ustore_16(uint16_t *uaddr, uint16_t val)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

int
_ustore_32(uint32_t *uaddr, uint32_t val)
{
	/* TODO(stub) */ __asm volatile("break");
	return EFAULT;
}

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

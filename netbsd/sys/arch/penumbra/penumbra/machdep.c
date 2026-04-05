/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent startup and utilities.
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

#include <uvm/uvm_extern.h>

#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/pcb.h>
#include <machine/frame.h>
#include <machine/vmparam.h>
#include <machine/pmap.h>

/* Forward declarations */
void	penumbra_init(void);
int	main(void);

/* Bootinfo pointer, saved by locore.S _start */
uint32_t penumbra_bootinfo;

/* Single CPU info structure (uniprocessor) */
struct cpu_info cpu_info_store;

/* Physical memory regions */
struct vm_map *phys_map;

/*
 * Early machine initialization.
 * Called from locore.S after BSS is zeroed.
 */
void
penumbra_init(void)
{

	/* Initialize the console so we can printf */
	consinit();

	printf("NetBSD/penumbra booting\n");

	/*
	 * TODO: Parse bootinfo to find memory regions
	 * TODO: Initialize UVM with physical pages
	 * TODO: Set up initial kernel pmap
	 * TODO: Call main()
	 */

	/* Initialize pmap (page table / TLB management) */
	pmap_bootstrap();

	/* Hand off to MI kernel main */
	main();
	/* NOTREACHED */
}

/*
 * Console initialization — called very early.
 * Sets up UART for printf.
 */
void
consinit(void)
{
	/*
	 * TODO: Initialize UART at 0xFF000000 (physical).
	 * For now, the bootloader has already set up the UART.
	 */
}

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

	/* TODO: actual reset — write to a reset sysreg or spin */
	for (;;)
		;
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

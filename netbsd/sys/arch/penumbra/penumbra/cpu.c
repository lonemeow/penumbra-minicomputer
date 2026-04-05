/*	$NetBSD$	*/

/*
 * Penumbra CPU device — attaches at mainbus.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/cpu.h>

#include <machine/cpu.h>

static int	cpu_match(device_t, cfdata_t, void *);
static void	cpu_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(cpu, 0,
    cpu_match, cpu_attach, NULL, NULL);

static int
cpu_match(device_t parent, cfdata_t cf, void *aux)
{

	return 1;
}

static void
cpu_attach(device_t parent, device_t self, void *aux)
{

	aprint_normal(": Penumbra 32-bit RISC\n");

	cpu_info_store.ci_dev = self;
	cpu_info_store.ci_cpuid = 0;
}

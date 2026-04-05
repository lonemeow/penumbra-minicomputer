/*	$NetBSD$	*/

/*
 * Penumbra autoconfiguration support.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/conf.h>

#include <machine/cpu.h>
#include <machine/intr.h>

/*
 * cpu_configure: called from configure() in init_main.c.
 * Kick off device autoconfiguration.
 */
void
cpu_configure(void)
{

	intr_init();

	(void)splhigh();
	if (config_rootfound("mainbus", NULL) == NULL)
		panic("no mainbus found");

	spl0();
}

/*
 * cpu_rootconf: called from configure() to set root device.
 */
void
cpu_rootconf(void)
{

	rootconf();
}

/*
 * device_register: called for each device during autoconf.
 */
void
device_register(device_t dev, void *aux)
{
	/* nothing yet */
}

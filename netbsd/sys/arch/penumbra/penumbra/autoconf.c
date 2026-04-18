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
#include <machine/bootinfo.h>

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
 *
 * If the bootloader passed a root device string via BTINFO_ROOTDEVICE,
 * parse it into booted_device + booted_partition so rootconf() can
 * auto-select root without prompting.
 */
void
cpu_rootconf(void)
{
	struct btinfo_rootdevice *bi;

	bi = lookup_bootinfo(BTINFO_ROOTDEVICE);
	if (bi != NULL) {
		/*
		 * Parse "ld0f" → device "ld0", partition 'f' - 'a' = 5.
		 * The last character is the partition letter if alphabetic.
		 */
		char devname[sizeof(bi->devname)];
		device_t dv;
		size_t len;

		strlcpy(devname, bi->devname, sizeof(devname));
		len = strlen(devname);

		if (len > 0 && devname[len - 1] >= 'a' &&
		    devname[len - 1] <= 'z') {
			booted_partition = devname[len - 1] - 'a';
			devname[len - 1] = '\0';
		}

		dv = device_find_by_xname(devname);
		if (dv != NULL) {
			booted_device = dv;
			aprint_normal("boot device: %s partition %c\n",
			    device_xname(booted_device),
			    'a' + booted_partition);
		}
	}

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

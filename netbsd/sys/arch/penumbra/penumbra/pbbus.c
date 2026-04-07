/*	$NetBSD$	*/

/*
 * Penumbra Bus bridge — enumerates devices from bootinfo.
 *
 * The ROM runs bus autoconfig (Zorro-style probe + address
 * assignment) and passes discovered devices to the kernel via
 * BTINFO_DEVICE entries in bootinfo.  The kernel cannot re-run
 * autoconfig without resetting the bus, so pbbus walks the
 * bootinfo and attaches a child for each non-memory device.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/pbbus.h>

static int	pbbus_match(device_t, cfdata_t, void *);
static void	pbbus_attach(device_t, device_t, void *);
static int	pbbus_print(void *, const char *);

CFATTACH_DECL_NEW(pbbus, 0,
    pbbus_match, pbbus_attach, NULL, NULL);

static int
pbbus_match(device_t parent, cfdata_t cf, void *aux)
{

	/* Only one pbbus, always matches at mainbus */
	return 1;
}

static void
pbbus_attach(device_t parent, device_t self, void *aux)
{
	aprint_normal("\n");

	struct btinfo_device *dev;
	for (int i = 0; (dev = lookup_bootinfo_n(BTINFO_DEVICE, i)); i++)
	{
		if (dev->cls != ACFG_CLASS_MEMORY)
		{
			struct pbbus_attach_args attach_args;
			attach_args.pb_addr = dev->addr;
			attach_args.pb_class = dev->cls;
			attach_args.pb_id = dev->id;
			attach_args.pb_name = dev->name;
			attach_args.pb_size = dev->size;
			config_found(self, &attach_args, pbbus_print, CFARGS_NONE);
		}
	}
}

/*
 * pbbus_print: format child device locator info for autoconf messages.
 * Produces output like: "pspi0 at pbbus0 addr 0xff001000"
 */
static int
pbbus_print(void *aux, const char *pnp)
{
	struct pbbus_attach_args *pa = aux;

	if (pnp != NULL)
		aprint_normal("%s at %s", pa->pb_name, pnp);
	aprint_normal(" addr 0x%08x", (unsigned int)pa->pb_addr);

	return UNCONF;
}

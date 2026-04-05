/*	$NetBSD$	*/

/*
 * Penumbra "mainbus" — root of the device tree.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

static int	mainbus_match(device_t, cfdata_t, void *);
static void	mainbus_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(mainbus, 0,
    mainbus_match, mainbus_attach, NULL, NULL);

static int
mainbus_match(device_t parent, cfdata_t cf, void *aux)
{

	return 1;
}

static void
mainbus_attach(device_t parent, device_t self, void *aux)
{

	aprint_normal("\n");

	/* Attach CPU */
	config_found(self, NULL, NULL, CFARGS_NONE);
}

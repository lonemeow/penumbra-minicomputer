/*	$NetBSD$	*/

/*
 * pusbhc — Penumbra CLASS_USBHC host controller.
 *
 * The controller executes one transaction-level USB operation at a
 * time (token, optional data packet, handshake) launched through the
 * TOKEN/XFER_CTRL registers; everything above a transaction — frames,
 * transfers, enumeration — is host software's job.  The driver will
 * implement the MI USB stack's bus interface (usbd_bus_methods with a
 * software root hub) in the style of dev/ic/sl811hs.c, which serves
 * the same one-transaction-at-a-time controller shape.
 *
 * Attachment skeleton: matches the autoconfig class, maps the
 * register window, and reports capabilities.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

/* ── CLASS_USBHC register map (word-strided) ─────────────────── */

#define USBHC_CAP		0x00
#define USBHC_IRQ_STATUS	0x04
#define USBHC_IRQ_ENABLE	0x08
#define USBHC_PORT_STATUS	0x0C
#define USBHC_PORT_CTRL		0x10
#define USBHC_FRAME		0x14
#define USBHC_TOKEN		0x18
#define USBHC_XFER_CTRL		0x1C
#define USBHC_XFER_STATUS	0x20
#define USBHC_DATA		0x40

#define USBHC_CAP_VERSION(c)	((c) & 0xff)
#define USBHC_CAP_BUFSZ(c)	(((c) >> 8) & 0xff)
#define USBHC_CAP_LS		0x00010000
#define USBHC_CAP_FS		0x00020000

#define USBHC_IRQ_XFER_DONE	0x00000001
#define USBHC_IRQ_PORT_CHANGE	0x00000002
#define USBHC_IRQ_SOF		0x00000004

struct pusbhc_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	uint32_t		sc_bufsz;	/* DATA buffer bytes */
};

#define PUSBHC_RD4(sc, r)		\
	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define PUSBHC_WR4(sc, r, v)	\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))

static int	pusbhc_match(device_t, cfdata_t, void *);
static void	pusbhc_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(pusbhc, sizeof(struct pusbhc_softc),
    pusbhc_match, pusbhc_attach, NULL, NULL);

static int
pusbhc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_USBHC;
}

static void
pusbhc_attach(device_t parent, device_t self, void *aux)
{
	struct pusbhc_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	uint32_t cap;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(pa->pb_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map registers: %d\n", error);
		return;
	}

	cap = PUSBHC_RD4(sc, USBHC_CAP);
	sc->sc_bufsz = USBHC_CAP_BUFSZ(cap);

	aprint_normal(": Penumbra USB host controller (v%u, buf %u%s%s)\n",
	    USBHC_CAP_VERSION(cap), sc->sc_bufsz,
	    (cap & USBHC_CAP_LS) ? ", LS" : "",
	    (cap & USBHC_CAP_FS) ? ", FS" : "");

	/*
	 * Quiesce: mask all interrupt sources and discard anything
	 * sticky from before the kernel took over.  The interrupt
	 * handler registers when the usbd bus interface lands.
	 */
	PUSBHC_WR4(sc, USBHC_IRQ_ENABLE, 0);
	PUSBHC_WR4(sc, USBHC_IRQ_STATUS,
	    USBHC_IRQ_XFER_DONE | USBHC_IRQ_PORT_CHANGE | USBHC_IRQ_SOF);
}

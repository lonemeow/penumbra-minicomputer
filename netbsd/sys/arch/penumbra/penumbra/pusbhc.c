/*	$NetBSD$	*/

/*
 * pusbhc — Penumbra CLASS_USBHC host controller.
 *
 * The controller executes one transaction-level USB operation at a
 * time (token, optional data packet, handshake) launched through the
 * TOKEN/XFER_CTRL registers; everything above a transaction — frames,
 * transfers, enumeration — is host software's job.  The driver
 * implements the MI USB stack's bus interface in the style of
 * dev/ic/sl811hs.c, which serves the same one-transaction-at-a-time
 * controller shape.
 *
 * This layer: bus methods and the software root hub.  The single
 * hardware port appears as a one-port root hub; usbroothub.c emulates
 * the hub device and calls ubm_rhctrl here for the hub-class and
 * per-port operations, which map onto PORT_CTRL/PORT_STATUS.  The
 * hardware coalesces connect, disconnect, and reset-complete into one
 * PORT_CHANGE event, while the hub protocol wants distinct C_PORT_*
 * change bits cleared individually — so the driver keeps its own
 * change latch: the interrupt handler diffs the connect view, and the
 * port-reset recipe contributes C_PORT_RESET directly.
 *
 * Device transfer pipes are not implemented yet; opening one fails
 * loudly.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/intr.h>
#include <sys/bus.h>

#include <machine/bootinfo.h>
#include <machine/pbbus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usbroothub.h>

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

#define USBHC_PS_CONNECT	0x00000001
#define USBHC_PS_ENABLED	0x00000002
#define USBHC_PS_RESET_ACTIVE	0x00000004
#define USBHC_PS_SUSPENDED	0x00000008
#define USBHC_PS_SPEED(ps)	(((ps) >> 4) & 3)
#define USBHC_SPEED_NONE	0
#define USBHC_SPEED_LOW		1
#define USBHC_SPEED_FULL	2

#define USBHC_PC_POWER		0x00000001
#define USBHC_PC_RESET		0x00000002
#define USBHC_PC_RUN		0x00000004

struct pusbhc_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	uint32_t		sc_bufsz;	/* DATA buffer bytes */

	struct usbd_bus		sc_bus;
	kmutex_t		sc_lock;	/* bus lock (IPL_SOFTUSB) */
	kmutex_t		sc_intr_lock;	/* registers + change latch */
	void			*sc_softint;
	device_t		sc_child;	/* usb* */

	struct usbd_xfer	*sc_intr_xfer;	/* root hub status pipe */
	uint16_t		sc_port_change;	/* accumulated UPS_C_* */
	bool			sc_connect_view; /* last reported CONNECT */
	bool			sc_dying;
};

#define PUSBHC_BUS2SC(bus)	((bus)->ub_hcpriv)
#define PUSBHC_PIPE2SC(pipe)	PUSBHC_BUS2SC((pipe)->up_dev->ud_bus)
#define PUSBHC_XFER2SC(xfer)	PUSBHC_BUS2SC((xfer)->ux_bus)

#define PUSBHC_RD4(sc, r)	\
	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define PUSBHC_WR4(sc, r, v)	\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))

static int	pusbhc_match(device_t, cfdata_t, void *);
static void	pusbhc_attach(device_t, device_t, void *);
static int	pusbhc_intr(void *);
static void	pusbhc_softintr(void *);

static usbd_status	pusbhc_open(struct usbd_pipe *);
static void		pusbhc_void_softint(void *);
static void		pusbhc_poll(struct usbd_bus *);
static struct usbd_xfer *pusbhc_allocx(struct usbd_bus *, unsigned int);
static void		pusbhc_freex(struct usbd_bus *, struct usbd_xfer *);
static void		pusbhc_abortx(struct usbd_xfer *);
static bool		pusbhc_dying(struct usbd_bus *);
static void		pusbhc_get_lock(struct usbd_bus *, kmutex_t **);
static int		pusbhc_roothub_ctrl(struct usbd_bus *,
			    usb_device_request_t *, void *, int);

static usbd_status	pusbhc_root_intr_transfer(struct usbd_xfer *);
static usbd_status	pusbhc_root_intr_start(struct usbd_xfer *);
static void		pusbhc_root_intr_abort(struct usbd_xfer *);
static void		pusbhc_root_intr_close(struct usbd_pipe *);
static void		pusbhc_root_intr_done(struct usbd_xfer *);
static void		pusbhc_noop_cleartoggle(struct usbd_pipe *);

static const struct usbd_bus_methods pusbhc_bus_methods = {
	.ubm_open =	pusbhc_open,
	.ubm_softint =	pusbhc_void_softint,
	.ubm_dopoll =	pusbhc_poll,
	.ubm_allocx =	pusbhc_allocx,
	.ubm_freex =	pusbhc_freex,
	.ubm_abortx =	pusbhc_abortx,
	.ubm_dying =	pusbhc_dying,
	.ubm_getlock =	pusbhc_get_lock,
	.ubm_rhctrl =	pusbhc_roothub_ctrl,
};

static const struct usbd_pipe_methods pusbhc_root_intr_methods = {
	.upm_transfer =	pusbhc_root_intr_transfer,
	.upm_start =	pusbhc_root_intr_start,
	.upm_abort =	pusbhc_root_intr_abort,
	.upm_close =	pusbhc_root_intr_close,
	.upm_cleartoggle = pusbhc_noop_cleartoggle,
	.upm_done =	pusbhc_root_intr_done,
};

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

	/* Quiesce: mask all sources, discard anything sticky. */
	PUSBHC_WR4(sc, USBHC_IRQ_ENABLE, 0);
	PUSBHC_WR4(sc, USBHC_IRQ_STATUS,
	    USBHC_IRQ_XFER_DONE | USBHC_IRQ_PORT_CHANGE | USBHC_IRQ_SOF);
	PUSBHC_WR4(sc, USBHC_PORT_CTRL, 0);

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_SOFTUSB);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_USB);

	sc->sc_softint = softint_establish(SOFTINT_USB | SOFTINT_MPSAFE,
	    pusbhc_softintr, sc);
	if (sc->sc_softint == NULL) {
		aprint_error_dev(self, "can't establish soft interrupt\n");
		return;
	}

	intr_establish_xname(0, IPL_USB, pusbhc_intr, sc,
	    device_xname(self));

	/* The port is unpowered after the quiesce: nothing connected. */
	sc->sc_connect_view = false;
	sc->sc_port_change = 0;

	sc->sc_bus.ub_hcpriv = sc;
	sc->sc_bus.ub_revision = USBREV_1_1;
	sc->sc_bus.ub_methods = &pusbhc_bus_methods;
	sc->sc_bus.ub_pipesize = sizeof(struct usbd_pipe);
	sc->sc_bus.ub_usedma = false;

	PUSBHC_WR4(sc, USBHC_IRQ_ENABLE, USBHC_IRQ_PORT_CHANGE);

	sc->sc_child = config_found(self, &sc->sc_bus, usbctlprint,
	    CFARGS_NONE);
}

/*
 * Hard interrupt, on the shared wired-OR line.  PORT_CHANGE is the
 * only enabled source at this layer; the coalesced event is resolved
 * against the last reported connect view — reset-complete changes are
 * accounted by the reset recipe itself, so only connect transitions
 * are latched here.
 */
static int
pusbhc_intr(void *arg)
{
	struct pusbhc_softc *sc = arg;
	uint32_t status;
	int claimed = 0;

	mutex_enter(&sc->sc_intr_lock);
	status = PUSBHC_RD4(sc, USBHC_IRQ_STATUS) &
	    PUSBHC_RD4(sc, USBHC_IRQ_ENABLE);
	if (status & USBHC_IRQ_PORT_CHANGE) {
		PUSBHC_WR4(sc, USBHC_IRQ_STATUS, USBHC_IRQ_PORT_CHANGE);

		bool connect = (PUSBHC_RD4(sc, USBHC_PORT_STATUS) &
		    USBHC_PS_CONNECT) != 0;
		if (connect != sc->sc_connect_view) {
			sc->sc_connect_view = connect;
			sc->sc_port_change |= UPS_C_CONNECT_STATUS;
		}

		claimed = 1;
		softint_schedule(sc->sc_softint);
	}
	mutex_exit(&sc->sc_intr_lock);

	return claimed;
}

/* Report an accumulated port change through the root hub status pipe. */
static void
pusbhc_softintr(void *arg)
{
	struct pusbhc_softc *sc = arg;
	struct usbd_xfer *xfer;
	uint16_t change;
	u_char *p;

	mutex_enter(&sc->sc_lock);
	xfer = sc->sc_intr_xfer;

	mutex_enter(&sc->sc_intr_lock);
	change = sc->sc_port_change;
	mutex_exit(&sc->sc_intr_lock);

	if (xfer != NULL && change != 0) {
		p = usbd_get_buffer(xfer);
		p[0] = 1 << 1;		/* bitmap: port 1 changed */
		xfer->ux_actlen = 1;
		xfer->ux_status = USBD_NORMAL_COMPLETION;
		usb_transfer_complete(xfer);
	}
	mutex_exit(&sc->sc_lock);
}

/* ── Bus methods ─────────────────────────────────────────────── */

static usbd_status
pusbhc_open(struct usbd_pipe *pipe)
{
	struct pusbhc_softc *sc = PUSBHC_PIPE2SC(pipe);
	struct usbd_device *dev = pipe->up_dev;
	usb_endpoint_descriptor_t *ed = pipe->up_endpoint->ue_edesc;

	if (sc->sc_dying)
		return USBD_IOERROR;

	if (dev->ud_addr == dev->ud_bus->ub_rhaddr) {
		switch (ed->bEndpointAddress) {
		case USB_CONTROL_ENDPOINT:
			pipe->up_methods = &roothub_ctrl_methods;
			break;
		case UE_DIR_IN | USBROOTHUB_INTR_ENDPT:
			pipe->up_methods = &pusbhc_root_intr_methods;
			break;
		default:
			return USBD_INVAL;
		}
		return USBD_NORMAL_COMPLETION;
	}

	aprint_error_dev(sc->sc_dev,
	    "device endpoint %#x: transfer pipes unsupported\n",
	    ed->bEndpointAddress);
	return USBD_IOERROR;
}

static void
pusbhc_void_softint(void *arg)
{
	/* Completions run through the driver's own soft interrupt. */
}

static void
pusbhc_poll(struct usbd_bus *bus)
{
	struct pusbhc_softc *sc = PUSBHC_BUS2SC(bus);

	pusbhc_intr(sc);
}

static struct usbd_xfer *
pusbhc_allocx(struct usbd_bus *bus, unsigned int nframes)
{
	struct usbd_xfer *xfer;

	xfer = kmem_zalloc(sizeof(*xfer), KM_SLEEP);
#ifdef DIAGNOSTIC
	xfer->ux_state = XFER_BUSY;
#endif
	return xfer;
}

static void
pusbhc_freex(struct usbd_bus *bus, struct usbd_xfer *xfer)
{
#ifdef DIAGNOSTIC
	KASSERTMSG(xfer->ux_state == XFER_BUSY ||
	    xfer->ux_status == USBD_NOT_STARTED,
	    "xfer %p state %d", xfer, xfer->ux_state);
	xfer->ux_state = XFER_FREE;
#endif
	kmem_free(xfer, sizeof(*xfer));
}

static void
pusbhc_abortx(struct usbd_xfer *xfer)
{

	/* Unreachable until device transfer pipes exist. */
	panic("pusbhc_abortx: no device transfer support");
}

static bool
pusbhc_dying(struct usbd_bus *bus)
{
	struct pusbhc_softc *sc = PUSBHC_BUS2SC(bus);

	return sc->sc_dying;
}

static void
pusbhc_get_lock(struct usbd_bus *bus, kmutex_t **lock)
{
	struct pusbhc_softc *sc = PUSBHC_BUS2SC(bus);

	*lock = &sc->sc_lock;
}

/* ── Root hub (ubm_rhctrl under usbroothub.c) ────────────────── */

/*
 * usbroothub.c has already served the standard requests into buf
 * (buflen ≥ 0) or marked the request unhandled (buflen < 0); this
 * hook implements the hub-class and per-port operations and passes
 * everything else through.  Returns the actual length, negative on
 * error.
 */
static int
pusbhc_roothub_ctrl(struct usbd_bus *bus, usb_device_request_t *req,
    void *buf, int buflen)
{
	struct pusbhc_softc *sc = PUSBHC_BUS2SC(bus);
	uint16_t len, value, index;
	int actlen;

	len = UGETW(req->wLength);
	value = UGETW(req->wValue);
	index = UGETW(req->wIndex);
	actlen = buflen;	/* pass through what usbroothub prepared */

#define C(x,y) ((x) | ((y) << 8))
	switch (C(req->bRequest, req->bmRequestType)) {
	case C(UR_GET_DESCRIPTOR, UT_READ_DEVICE):
		switch (value) {
		case (UDESC_STRING << 8) | 1:
			actlen = usb_makestrdesc(buf, len, "Penumbra");
			break;
		case (UDESC_STRING << 8) | 2:
			actlen = usb_makestrdesc(buf, len, "USBHC root hub");
			break;
		}
		break;

	case C(UR_GET_DESCRIPTOR, UT_READ_CLASS_DEVICE): {
		/*
		 * The default one-port hub descriptor is already in
		 * buf; correct the characteristics — POWER is a real
		 * per-port switch and there is no overcurrent sensing.
		 */
		usb_hub_descriptor_t hubd;
		size_t patchlen = uimin((size_t)buflen, sizeof(hubd));

		memcpy(&hubd, buf, patchlen);
		USETW(hubd.wHubCharacteristics,
		    UHD_PWR_INDIVIDUAL | UHD_OC_NONE);
		memcpy(buf, &hubd, patchlen);
		break;
	}

	case C(UR_GET_STATUS, UT_READ_CLASS_DEVICE):
		/* Hub status: no local power source, no overcurrent. */
		if (len < 4)
			return -1;
		memset(buf, 0, 4);
		actlen = 4;
		break;

	case C(UR_GET_STATUS, UT_READ_CLASS_OTHER): {
		usb_port_status_t ps;
		uint32_t regs, ctrl;
		uint16_t status, change;

		if (index != 1 || len < sizeof(ps))
			return -1;

		mutex_enter(&sc->sc_intr_lock);
		regs = PUSBHC_RD4(sc, USBHC_PORT_STATUS);
		ctrl = PUSBHC_RD4(sc, USBHC_PORT_CTRL);
		change = sc->sc_port_change;
		mutex_exit(&sc->sc_intr_lock);

		status = 0;
		if (ctrl & USBHC_PC_POWER)
			status |= UPS_PORT_POWER;
		if (regs & USBHC_PS_CONNECT)
			status |= UPS_CURRENT_CONNECT_STATUS;
		if (regs & USBHC_PS_ENABLED)
			status |= UPS_PORT_ENABLED;
		if (regs & USBHC_PS_RESET_ACTIVE)
			status |= UPS_RESET;
		if (regs & USBHC_PS_SUSPENDED)
			status |= UPS_SUSPEND;
		/* Full speed is the unmarked default in a USB 1.1 hub. */
		if (USBHC_PS_SPEED(regs) == USBHC_SPEED_LOW)
			status |= UPS_LOW_SPEED;

		USETW(ps.wPortStatus, status);
		USETW(ps.wPortChange, change);
		memcpy(buf, &ps, sizeof(ps));
		actlen = sizeof(ps);
		break;
	}

	case C(UR_SET_FEATURE, UT_WRITE_CLASS_OTHER):
		if (index != 1)
			return -1;
		switch (value) {
		case UHF_PORT_POWER:
			mutex_enter(&sc->sc_intr_lock);
			PUSBHC_WR4(sc, USBHC_PORT_CTRL,
			    USBHC_PC_POWER | USBHC_PC_RUN);
			mutex_exit(&sc->sc_intr_lock);
			break;
		case UHF_PORT_RESET: {
			int i;

			mutex_enter(&sc->sc_intr_lock);
			PUSBHC_WR4(sc, USBHC_PORT_CTRL,
			    USBHC_PC_POWER | USBHC_PC_RUN | USBHC_PC_RESET);
			mutex_exit(&sc->sc_intr_lock);

			usb_delay_ms(&sc->sc_bus, USB_PORT_ROOT_RESET_DELAY);

			mutex_enter(&sc->sc_intr_lock);
			PUSBHC_WR4(sc, USBHC_PORT_CTRL,
			    USBHC_PC_POWER | USBHC_PC_RUN);
			for (i = 0; i < 100; i++) {
				if (PUSBHC_RD4(sc, USBHC_PORT_STATUS) &
				    USBHC_PS_ENABLED)
					break;
				DELAY(100);
			}
			if (i < 100)
				sc->sc_port_change |= UPS_C_PORT_RESET;
			mutex_exit(&sc->sc_intr_lock);

			if (i == 100) {
				aprint_error_dev(sc->sc_dev,
				    "port reset did not enable the port\n");
				return -1;
			}
			break;
		}
		default:
			return -1;
		}
		actlen = 0;
		break;

	case C(UR_CLEAR_FEATURE, UT_WRITE_CLASS_OTHER):
		if (index != 1)
			return -1;
		switch (value) {
		case UHF_PORT_POWER:
			mutex_enter(&sc->sc_intr_lock);
			PUSBHC_WR4(sc, USBHC_PORT_CTRL, 0);
			mutex_exit(&sc->sc_intr_lock);
			break;
		case UHF_C_PORT_CONNECTION:
			mutex_enter(&sc->sc_intr_lock);
			sc->sc_port_change &= ~UPS_C_CONNECT_STATUS;
			mutex_exit(&sc->sc_intr_lock);
			break;
		case UHF_C_PORT_RESET:
			mutex_enter(&sc->sc_intr_lock);
			sc->sc_port_change &= ~UPS_C_PORT_RESET;
			mutex_exit(&sc->sc_intr_lock);
			break;
		case UHF_C_PORT_ENABLE:
			/* Never latched; clearing it is a no-op. */
			break;
		default:
			return -1;
		}
		actlen = 0;
		break;

	default:
		break;
	}
#undef C

	return actlen;
}

/* ── Root hub interrupt pipe ─────────────────────────────────── */

static usbd_status
pusbhc_root_intr_transfer(struct usbd_xfer *xfer)
{

	/* Pipe isn't running, start first */
	return pusbhc_root_intr_start(SIMPLEQ_FIRST(&xfer->ux_pipe->up_queue));
}

static usbd_status
pusbhc_root_intr_start(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc = PUSBHC_XFER2SC(xfer);

	KASSERT(sc->sc_bus.ub_usepolling || mutex_owned(&sc->sc_lock));

	if (sc->sc_dying)
		return USBD_IOERROR;

	KASSERT(sc->sc_intr_xfer == NULL);
	sc->sc_intr_xfer = xfer;
	xfer->ux_status = USBD_IN_PROGRESS;

	return USBD_IN_PROGRESS;
}

static void
pusbhc_root_intr_abort(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc = PUSBHC_XFER2SC(xfer);

	KASSERT(mutex_owned(&sc->sc_lock));

	/* If the xfer has already completed, nothing to do here. */
	if (sc->sc_intr_xfer == NULL)
		return;

	KASSERT(sc->sc_intr_xfer == xfer);
	KASSERT(xfer->ux_status == USBD_IN_PROGRESS);
	xfer->ux_status = USBD_CANCELLED;
	usb_transfer_complete(xfer);
}

static void
pusbhc_root_intr_close(struct usbd_pipe *pipe)
{

	/* The abort that precedes a close has already detached it. */
}

static void
pusbhc_root_intr_done(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc = PUSBHC_XFER2SC(xfer);

	KASSERT(mutex_owned(&sc->sc_lock));
	KASSERT(sc->sc_intr_xfer == xfer);
	sc->sc_intr_xfer = NULL;
}

static void
pusbhc_noop_cleartoggle(struct usbd_pipe *pipe)
{
}

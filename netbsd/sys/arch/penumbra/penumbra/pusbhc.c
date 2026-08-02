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
 * Device transfers run through a small scheduling engine: each open
 * pipe executes at most one transfer at a time, a transfer advances
 * one hardware transaction at a time, and the single port serves one
 * pipe's transaction at a time from a ready queue.  The hard
 * interrupt handler advances the engine (XFER_DONE) and paces
 * interrupt-endpoint polling (SOF); finished transfers are posted to
 * the soft interrupt, which delivers completions under the bus lock.
 * All packet data stages through the controller's DATA buffer, so
 * nothing outside the completion path touches a caller's memory.
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

#define USBHC_TOKEN_PID_SETUP	0
#define USBHC_TOKEN_PID_OUT	1
#define USBHC_TOKEN_PID_IN	2
#define USBHC_TOKEN_BUILD(pid, addr, ep, toggle) \
	((pid) | ((addr) << 4) | ((ep) << 11) | ((toggle) ? (1 << 16) : 0))

#define USBHC_XC_START		(1 << 16)

#define USBHC_XS_DONE		0x00000001
#define USBHC_XS_RESULT(xs)	(((xs) >> 1) & 7)
#define USBHC_RESULT_ACK	0
#define USBHC_RESULT_NAK	1
#define USBHC_RESULT_STALL	2
#define USBHC_RESULT_TIMEOUT	3
#define USBHC_RESULT_ERROR	4
#define USBHC_RESULT_OVERFLOW	5
#define USBHC_XS_RXTOGGLE	0x00000010
#define USBHC_XS_RXLEN(xs)	(((xs) >> 8) & 0x7f)

/*
 * A device pipe executes at most one transfer at a time (the MI stack
 * starts the next queued transfer from the completion of the current
 * one), and a transfer advances one hardware transaction at a time.
 * The pipe's engine state says where its current transfer stands:
 * READY pipes queue on sc_ready for the single port; the ACTIVE
 * pipe's transaction is in the hardware; WAITFRAME pipes sit out a
 * polling interval on sc_wait; DONE pipes wait on sc_done for the
 * soft interrupt to deliver their completion.
 */
enum pusbhc_pipe_state {
	PUSBHC_PIPE_IDLE,
	PUSBHC_PIPE_READY,
	PUSBHC_PIPE_ACTIVE,
	PUSBHC_PIPE_WAITFRAME,
	PUSBHC_PIPE_DONE,
};

/* Control transfers walk SETUP → DATA → STATUS; others are all DATA. */
enum pusbhc_xfer_stage {
	PUSBHC_STAGE_SETUP,
	PUSBHC_STAGE_DATA,
	PUSBHC_STAGE_STATUS,
};

struct pusbhc_pipe {
	struct usbd_pipe	pp_pipe;	/* MI view; must be first */
	SIMPLEQ_ENTRY(pusbhc_pipe) pp_q;	/* ready / wait / done */
	struct usbd_xfer	*pp_xfer;	/* transfer being executed */
	enum pusbhc_pipe_state	pp_state;
	enum pusbhc_xfer_stage	pp_stage;
	uint8_t			pp_type;	/* UE_CONTROL/_INTERRUPT/_BULK */
	bool			pp_toggle;	/* next data toggle */
	bool			pp_zlp;		/* OUT owes a closing ZLP */
	uint8_t			pp_lastlen;	/* bytes in the launched txn */
	uint8_t			pp_errors;	/* consecutive bus errors */
	uint16_t		pp_interval;	/* poll interval, frames */
	uint16_t		pp_countdown;	/* WAITFRAME frames left */
	uint32_t		pp_offset;	/* ux_buf bytes completed */
	usbd_status		pp_status;	/* completion status for DONE */
};

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

	/* Transfer engine, under sc_intr_lock. */
	SIMPLEQ_HEAD(, pusbhc_pipe) sc_ready;	/* pipes wanting the port */
	SIMPLEQ_HEAD(, pusbhc_pipe) sc_wait;	/* pipes pacing an interval */
	SIMPLEQ_HEAD(, pusbhc_pipe) sc_done;	/* completions to deliver */
	struct pusbhc_pipe	*sc_active;	/* pipe owning the hardware */
	bool			sc_orphan;	/* active txn lost its pipe */
};

#define PUSBHC_BUS2SC(bus)	((bus)->ub_hcpriv)
#define PUSBHC_PIPE2SC(pipe)	PUSBHC_BUS2SC((pipe)->up_dev->ud_bus)
#define PUSBHC_XFER2SC(xfer)	PUSBHC_BUS2SC((xfer)->ux_bus)
#define PUSBHC_PIPE2PP(pipe)	((struct pusbhc_pipe *)(pipe))
#define PUSBHC_XFER2PP(xfer)	PUSBHC_PIPE2PP((xfer)->ux_pipe)

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

static void	pusbhc_write_data(struct pusbhc_softc *, const uint8_t *,
		    u_int);
static void	pusbhc_read_data(struct pusbhc_softc *, uint8_t *, u_int);
static bool	pusbhc_stage_isread(struct pusbhc_pipe *);
static void	pusbhc_update_irqmask(struct pusbhc_softc *);
static void	pusbhc_launch(struct pusbhc_softc *);
static void	pusbhc_kick(struct pusbhc_softc *);
static void	pusbhc_pipe_ready(struct pusbhc_softc *,
		    struct pusbhc_pipe *);
static void	pusbhc_pipe_wait(struct pusbhc_softc *,
		    struct pusbhc_pipe *);
static void	pusbhc_complete(struct pusbhc_softc *, struct pusbhc_pipe *,
		    usbd_status);
static void	pusbhc_advance(struct pusbhc_softc *, struct pusbhc_pipe *,
		    uint32_t);
static void	pusbhc_xfer_done(struct pusbhc_softc *, uint32_t);
static void	pusbhc_sof(struct pusbhc_softc *);
static void	pusbhc_drain_done(struct pusbhc_softc *);

static usbd_status	pusbhc_device_transfer(struct usbd_xfer *);
static usbd_status	pusbhc_device_start(struct usbd_xfer *);
static void		pusbhc_device_abort(struct usbd_xfer *);
static void		pusbhc_device_close(struct usbd_pipe *);
static void		pusbhc_device_cleartoggle(struct usbd_pipe *);
static void		pusbhc_device_done(struct usbd_xfer *);

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

/* One method set serves control, interrupt, and bulk device pipes;
 * the engine branches on the endpoint type where behavior differs. */
static const struct usbd_pipe_methods pusbhc_device_methods = {
	.upm_transfer =	pusbhc_device_transfer,
	.upm_start =	pusbhc_device_start,
	.upm_abort =	pusbhc_device_abort,
	.upm_close =	pusbhc_device_close,
	.upm_cleartoggle = pusbhc_device_cleartoggle,
	.upm_done =	pusbhc_device_done,
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

	SIMPLEQ_INIT(&sc->sc_ready);
	SIMPLEQ_INIT(&sc->sc_wait);
	SIMPLEQ_INIT(&sc->sc_done);
	sc->sc_active = NULL;
	sc->sc_orphan = false;

	sc->sc_bus.ub_hcpriv = sc;
	sc->sc_bus.ub_revision = USBREV_1_1;
	sc->sc_bus.ub_methods = &pusbhc_bus_methods;
	sc->sc_bus.ub_pipesize = sizeof(struct pusbhc_pipe);
	sc->sc_bus.ub_usedma = false;

	pusbhc_update_irqmask(sc);

	sc->sc_child = config_found(self, &sc->sc_bus, usbctlprint,
	    CFARGS_NONE);
}

/*
 * Hard interrupt, on the shared wired-OR line.  XFER_DONE advances
 * the transfer engine; SOF ages the interval-pacing pipes.  For
 * PORT_CHANGE, the coalesced event is resolved against the last
 * reported connect view — reset-complete changes are accounted by the
 * reset recipe itself, so only connect transitions are latched here.
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

	if (status & USBHC_IRQ_XFER_DONE) {
		uint32_t xs = PUSBHC_RD4(sc, USBHC_XFER_STATUS);

		PUSBHC_WR4(sc, USBHC_IRQ_STATUS, USBHC_IRQ_XFER_DONE);
		if (sc->sc_orphan) {
			/* Aborted mid-flight; the port is free again. */
			sc->sc_orphan = false;
			pusbhc_kick(sc);
		} else if (sc->sc_active != NULL) {
			pusbhc_xfer_done(sc, xs);
		}
		claimed = 1;
	}

	if (status & USBHC_IRQ_SOF) {
		PUSBHC_WR4(sc, USBHC_IRQ_STATUS, USBHC_IRQ_SOF);
		pusbhc_sof(sc);
		claimed = 1;
	}

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

/*
 * Deliver posted transfer completions under the bus lock (or with the
 * bus in polling mode).  A completion claimed by a concurrent abort
 * or MI timeout never reaches sc_done — abortx detaches it — so
 * trycomplete failing here is only a guard, not an expected path.
 */
static void
pusbhc_drain_done(struct pusbhc_softc *sc)
{
	struct pusbhc_pipe *pp;
	struct usbd_xfer *xfer;
	usbd_status status;

	KASSERT(sc->sc_bus.ub_usepolling || mutex_owned(&sc->sc_lock));

	for (;;) {
		mutex_enter(&sc->sc_intr_lock);
		pp = SIMPLEQ_FIRST(&sc->sc_done);
		if (pp != NULL) {
			SIMPLEQ_REMOVE_HEAD(&sc->sc_done, pp_q);
			xfer = pp->pp_xfer;
			status = pp->pp_status;
			pp->pp_state = PUSBHC_PIPE_IDLE;
			pp->pp_xfer = NULL;
		}
		mutex_exit(&sc->sc_intr_lock);
		if (pp == NULL)
			break;

		if (!usbd_xfer_trycomplete(xfer))
			continue;
		xfer->ux_status = status;
		usb_transfer_complete(xfer);
	}
}

/* Deliver transfer completions and accumulated port changes. */
static void
pusbhc_softintr(void *arg)
{
	struct pusbhc_softc *sc = arg;
	struct usbd_xfer *xfer;
	uint16_t change;
	u_char *p;

	mutex_enter(&sc->sc_lock);
	pusbhc_drain_done(sc);
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

	switch (ed->bmAttributes & UE_XFERTYPE) {
	case UE_CONTROL:
	case UE_INTERRUPT:
	case UE_BULK:
		break;
	default:
		aprint_error_dev(sc->sc_dev,
		    "endpoint %#x: unsupported transfer type %#x\n",
		    ed->bEndpointAddress, ed->bmAttributes & UE_XFERTYPE);
		return USBD_INVAL;
	}
	if (UGETW(ed->wMaxPacketSize) > sc->sc_bufsz) {
		aprint_error_dev(sc->sc_dev,
		    "endpoint %#x: max packet %u exceeds the %u-byte "
		    "data buffer\n", ed->bEndpointAddress,
		    UGETW(ed->wMaxPacketSize), sc->sc_bufsz);
		return USBD_INVAL;
	}

	struct pusbhc_pipe *pp = PUSBHC_PIPE2PP(pipe);

	pp->pp_type = ed->bmAttributes & UE_XFERTYPE;
	pp->pp_state = PUSBHC_PIPE_IDLE;
	pp->pp_xfer = NULL;
	pp->pp_toggle = false;
	/* bInterval counts 1 ms frames at low/full speed; 0 and 1 both
	 * mean every frame. */
	pp->pp_interval = uimax(1, ed->bInterval);
	pipe->up_methods = &pusbhc_device_methods;
	return USBD_NORMAL_COMPLETION;
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
	if (sc->sc_bus.ub_usepolling)
		pusbhc_drain_done(sc);
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

/*
 * Detach the claimed transfer from the engine.  usbd_xfer_abort (or
 * the MI timeout path) owns the completion; the engine must only
 * forget the transfer.  A transaction already launched cannot be
 * recalled — the pipe is disowned instead and the port freed when its
 * XFER_DONE arrives; nothing references the caller's memory
 * meanwhile, because all packet data stages through the controller's
 * DATA buffer.
 */
static void
pusbhc_abortx(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc = PUSBHC_XFER2SC(xfer);
	struct pusbhc_pipe *pp = PUSBHC_XFER2PP(xfer);

	KASSERT(mutex_owned(&sc->sc_lock));

	mutex_enter(&sc->sc_intr_lock);
	KASSERT(pp->pp_xfer == xfer);
	switch (pp->pp_state) {
	case PUSBHC_PIPE_READY:
		SIMPLEQ_REMOVE(&sc->sc_ready, pp, pusbhc_pipe, pp_q);
		break;
	case PUSBHC_PIPE_ACTIVE:
		sc->sc_active = NULL;
		sc->sc_orphan = true;
		break;
	case PUSBHC_PIPE_WAITFRAME:
		SIMPLEQ_REMOVE(&sc->sc_wait, pp, pusbhc_pipe, pp_q);
		pusbhc_update_irqmask(sc);
		break;
	case PUSBHC_PIPE_DONE:
		SIMPLEQ_REMOVE(&sc->sc_done, pp, pusbhc_pipe, pp_q);
		break;
	case PUSBHC_PIPE_IDLE:
		/* The claim races nothing else; IDLE cannot happen. */
		panic("pusbhc_abortx: idle pipe");
	}
	pp->pp_state = PUSBHC_PIPE_IDLE;
	pp->pp_xfer = NULL;
	mutex_exit(&sc->sc_intr_lock);
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

/* ── Transfer engine (under sc_intr_lock) ────────────────────── */

/*
 * The DATA buffer is word-access-only.  Buffers always arrive
 * word-aligned in practice — a non-DMA bus draws ux_buf from
 * kmem_alloc, whose smallest guaranteed alignment is 8 — and
 * intra-transfer offsets advance in max-packet multiples, so the
 * byte path runs only for sub-word tails and odd-sized interrupt
 * endpoints.  The word run goes through the region op; the byte
 * tail is assembled explicitly in the register contract's
 * little-endian lane order.
 */
static void
pusbhc_write_data(struct pusbhc_softc *sc, const uint8_t *buf, u_int len)
{
	uint32_t w;
	u_int off = 0;

	if (((uintptr_t)buf & 3) == 0) {
		bus_space_write_region_4(sc->sc_iot, sc->sc_ioh, USBHC_DATA,
		    (const uint32_t *)buf, len / 4);
		off = len & ~3u;
	}
	for (; off < len; off += 4) {
		w = buf[off];
		if (off + 1 < len)
			w |= (uint32_t)buf[off + 1] << 8;
		if (off + 2 < len)
			w |= (uint32_t)buf[off + 2] << 16;
		if (off + 3 < len)
			w |= (uint32_t)buf[off + 3] << 24;
		PUSBHC_WR4(sc, USBHC_DATA + off, w);
	}
}

static void
pusbhc_read_data(struct pusbhc_softc *sc, uint8_t *buf, u_int len)
{
	uint32_t w;
	u_int off = 0;

	if (((uintptr_t)buf & 3) == 0) {
		bus_space_read_region_4(sc->sc_iot, sc->sc_ioh, USBHC_DATA,
		    (uint32_t *)buf, len / 4);
		off = len & ~3u;
	}
	for (; off < len; off += 4) {
		w = PUSBHC_RD4(sc, USBHC_DATA + off);
		buf[off] = w;
		if (off + 1 < len)
			buf[off + 1] = w >> 8;
		if (off + 2 < len)
			buf[off + 2] = w >> 16;
		if (off + 3 < len)
			buf[off + 3] = w >> 24;
	}
}

/* Whether the pipe's data stage moves device-to-host. */
static bool
pusbhc_stage_isread(struct pusbhc_pipe *pp)
{
	if (pp->pp_type == UE_CONTROL)
		return (pp->pp_xfer->ux_request.bmRequestType & UT_READ) != 0;
	return (pp->pp_pipe.up_endpoint->ue_edesc->bEndpointAddress &
	    UE_DIR_IN) != 0;
}

/* SOF interrupts run only while some pipe is pacing an interval. */
static void
pusbhc_update_irqmask(struct pusbhc_softc *sc)
{
	uint32_t mask = USBHC_IRQ_XFER_DONE | USBHC_IRQ_PORT_CHANGE;

	if (!SIMPLEQ_EMPTY(&sc->sc_wait))
		mask |= USBHC_IRQ_SOF;
	PUSBHC_WR4(sc, USBHC_IRQ_ENABLE, mask);
}

/* Program and start the ACTIVE pipe's next transaction. */
static void
pusbhc_launch(struct pusbhc_softc *sc)
{
	struct pusbhc_pipe *pp = sc->sc_active;
	struct usbd_xfer *xfer = pp->pp_xfer;
	usb_endpoint_descriptor_t *ed = pp->pp_pipe.up_endpoint->ue_edesc;
	uint32_t pid;
	bool toggle;
	u_int len;

	KASSERT(mutex_owned(&sc->sc_intr_lock));
	KASSERT(pp->pp_state == PUSBHC_PIPE_ACTIVE);

	switch (pp->pp_stage) {
	case PUSBHC_STAGE_SETUP:
		pid = USBHC_TOKEN_PID_SETUP;
		toggle = false;
		len = sizeof(xfer->ux_request);
		pusbhc_write_data(sc,
		    (const uint8_t *)&xfer->ux_request, len);
		break;
	case PUSBHC_STAGE_DATA:
		toggle = pp->pp_toggle;
		len = uimin(UGETW(ed->wMaxPacketSize),
		    xfer->ux_length - pp->pp_offset);
		if (pusbhc_stage_isread(pp)) {
			pid = USBHC_TOKEN_PID_IN;
		} else {
			pid = USBHC_TOKEN_PID_OUT;
			pusbhc_write_data(sc,
			    (const uint8_t *)xfer->ux_buf + pp->pp_offset,
			    len);
		}
		break;
	case PUSBHC_STAGE_STATUS:
	default:
		/* Zero-length, opposite the data stage, always DATA1. */
		pid = (xfer->ux_length > 0 && pusbhc_stage_isread(pp)) ?
		    USBHC_TOKEN_PID_OUT : USBHC_TOKEN_PID_IN;
		toggle = true;
		len = 0;
		break;
	}

	pp->pp_lastlen = len;
	PUSBHC_WR4(sc, USBHC_TOKEN, USBHC_TOKEN_BUILD(pid,
	    pp->pp_pipe.up_dev->ud_addr, UE_GET_ADDR(ed->bEndpointAddress),
	    toggle));
	PUSBHC_WR4(sc, USBHC_XFER_CTRL, USBHC_XC_START | len);
}

/* Grant the port to the first READY pipe once the hardware is free. */
static void
pusbhc_kick(struct pusbhc_softc *sc)
{
	struct pusbhc_pipe *pp;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	if (sc->sc_active != NULL || sc->sc_orphan)
		return;
	pp = SIMPLEQ_FIRST(&sc->sc_ready);
	if (pp == NULL)
		return;
	SIMPLEQ_REMOVE_HEAD(&sc->sc_ready, pp_q);
	pp->pp_state = PUSBHC_PIPE_ACTIVE;
	sc->sc_active = pp;
	pusbhc_launch(sc);
}

/* Line the pipe up behind the other READY pipes for the port. */
static void
pusbhc_pipe_ready(struct pusbhc_softc *sc, struct pusbhc_pipe *pp)
{
	KASSERT(mutex_owned(&sc->sc_intr_lock));
	KASSERT(sc->sc_active != pp);

	pp->pp_state = PUSBHC_PIPE_READY;
	SIMPLEQ_INSERT_TAIL(&sc->sc_ready, pp, pp_q);
	pusbhc_kick(sc);
}

/* Sit out pp_interval frames; the SOF tick re-readies the pipe. */
static void
pusbhc_pipe_wait(struct pusbhc_softc *sc, struct pusbhc_pipe *pp)
{
	KASSERT(mutex_owned(&sc->sc_intr_lock));
	KASSERT(sc->sc_active != pp);

	pp->pp_state = PUSBHC_PIPE_WAITFRAME;
	pp->pp_countdown = pp->pp_interval;
	SIMPLEQ_INSERT_TAIL(&sc->sc_wait, pp, pp_q);
	pusbhc_update_irqmask(sc);
	pusbhc_kick(sc);
}

/* Post the ACTIVE pipe's transfer for completion and free the port. */
static void
pusbhc_complete(struct pusbhc_softc *sc, struct pusbhc_pipe *pp,
    usbd_status status)
{
	KASSERT(mutex_owned(&sc->sc_intr_lock));
	KASSERT(sc->sc_active == pp);

	sc->sc_active = NULL;
	pp->pp_state = PUSBHC_PIPE_DONE;
	pp->pp_status = status;
	pp->pp_xfer->ux_actlen = pp->pp_offset;
	SIMPLEQ_INSERT_TAIL(&sc->sc_done, pp, pp_q);
	softint_schedule(sc->sc_softint);
	pusbhc_kick(sc);
}

/*
 * An ACK moves the transfer forward: SETUP hands off to the data or
 * status stage, a data-stage packet advances the buffer walk, and the
 * status handshake finishes the control transfer.
 */
static void
pusbhc_advance(struct pusbhc_softc *sc, struct pusbhc_pipe *pp, uint32_t xs)
{
	struct usbd_xfer *xfer = pp->pp_xfer;
	bool isread = pusbhc_stage_isread(pp);
	bool short_pkt;
	u_int rxlen;

	switch (pp->pp_stage) {
	case PUSBHC_STAGE_SETUP:
		/* The data stage starts on DATA1. */
		pp->pp_toggle = true;
		pp->pp_stage = (xfer->ux_length > 0) ?
		    PUSBHC_STAGE_DATA : PUSBHC_STAGE_STATUS;
		pusbhc_launch(sc);
		break;

	case PUSBHC_STAGE_DATA:
		if (isread) {
			/*
			 * A toggle mismatch is the device retransmitting
			 * a packet whose ACK it lost: discard the data
			 * and re-run the transaction — the repeated ACK
			 * alone resynchronizes it.
			 */
			if (((xs & USBHC_XS_RXTOGGLE) != 0) !=
			    pp->pp_toggle) {
				pusbhc_launch(sc);
				break;
			}
			rxlen = USBHC_XS_RXLEN(xs);
			pusbhc_read_data(sc,
			    (uint8_t *)xfer->ux_buf + pp->pp_offset, rxlen);
			pp->pp_offset += rxlen;
			short_pkt = rxlen < pp->pp_lastlen;
		} else {
			pp->pp_offset += pp->pp_lastlen;
			if (pp->pp_lastlen == 0)
				pp->pp_zlp = false;
			short_pkt = false;
		}
		pp->pp_toggle = !pp->pp_toggle;

		if ((pp->pp_offset < xfer->ux_length && !short_pkt) ||
		    (!isread && pp->pp_zlp)) {
			pusbhc_launch(sc);
			break;
		}
		if (pp->pp_type == UE_CONTROL) {
			pp->pp_stage = PUSBHC_STAGE_STATUS;
			pusbhc_launch(sc);
			break;
		}
		pusbhc_complete(sc, pp,
		    (isread && pp->pp_offset < xfer->ux_length &&
		    (xfer->ux_flags & USBD_SHORT_XFER_OK) == 0) ?
		    USBD_SHORT_XFER : USBD_NORMAL_COMPLETION);
		break;

	case PUSBHC_STAGE_STATUS:
		/*
		 * The status handshake closes the control transfer; a
		 * data stage shorter than wLength is normal (wLength
		 * is an upper bound), so actlen tells the caller.
		 */
		pusbhc_complete(sc, pp, USBD_NORMAL_COMPLETION);
		break;
	}
}

/*
 * XFER_DONE for the ACTIVE pipe's transaction: classify RESULT and
 * advance, retry, or finish the transfer.
 */
static void
pusbhc_xfer_done(struct pusbhc_softc *sc, uint32_t xs)
{
	struct pusbhc_pipe *pp = sc->sc_active;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	switch (USBHC_XS_RESULT(xs)) {
	case USBHC_RESULT_ACK:
		pp->pp_errors = 0;
		pusbhc_advance(sc, pp, xs);
		break;

	case USBHC_RESULT_NAK:
		/*
		 * A NAK never advances the transfer; the same
		 * transaction runs again.  Interrupt endpoints poll at
		 * their declared interval — an idle HID device NAKs
		 * forever, so pacing is their steady state.  Control
		 * and bulk retry through the ready queue: alone on the
		 * bus that re-grants the port immediately, under
		 * contention it round-robins.
		 */
		sc->sc_active = NULL;
		if (pp->pp_type == UE_INTERRUPT)
			pusbhc_pipe_wait(sc, pp);
		else
			pusbhc_pipe_ready(sc, pp);
		break;

	case USBHC_RESULT_STALL:
		pusbhc_complete(sc, pp, USBD_STALLED);
		break;

	case USBHC_RESULT_TIMEOUT:
	case USBHC_RESULT_ERROR:
	case USBHC_RESULT_OVERFLOW:
	default:
		/* Three consecutive failures kill the transfer, per
		 * the USB error-recovery rule. */
		if (++pp->pp_errors < 3) {
			pusbhc_launch(sc);
			break;
		}
		pusbhc_complete(sc, pp,
		    USBHC_XS_RESULT(xs) == USBHC_RESULT_TIMEOUT ?
		    USBD_TIMEOUT : USBD_IOERROR);
		break;
	}
}

/* One frame elapsed: age the pacing pipes, re-readying the expired. */
static void
pusbhc_sof(struct pusbhc_softc *sc)
{
	struct pusbhc_pipe *pp, *next;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	SIMPLEQ_FOREACH_SAFE(pp, &sc->sc_wait, pp_q, next) {
		if (--pp->pp_countdown > 0)
			continue;
		SIMPLEQ_REMOVE(&sc->sc_wait, pp, pusbhc_pipe, pp_q);
		pusbhc_pipe_ready(sc, pp);
	}
	pusbhc_update_irqmask(sc);
}

/* ── Device pipes ────────────────────────────────────────────── */

static usbd_status
pusbhc_device_transfer(struct usbd_xfer *xfer)
{

	/* Pipe isn't running, start first */
	return pusbhc_device_start(SIMPLEQ_FIRST(&xfer->ux_pipe->up_queue));
}

static usbd_status
pusbhc_device_start(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc = PUSBHC_XFER2SC(xfer);
	struct pusbhc_pipe *pp = PUSBHC_XFER2PP(xfer);
	u_int mps =
	    UGETW(pp->pp_pipe.up_endpoint->ue_edesc->wMaxPacketSize);

	KASSERT(sc->sc_bus.ub_usepolling || mutex_owned(&sc->sc_lock));

	if (sc->sc_dying)
		return USBD_IOERROR;

	mutex_enter(&sc->sc_intr_lock);
	KASSERT(pp->pp_state == PUSBHC_PIPE_IDLE);
	pp->pp_xfer = xfer;
	pp->pp_offset = 0;
	pp->pp_errors = 0;
	if (pp->pp_type == UE_CONTROL) {
		pp->pp_stage = PUSBHC_STAGE_SETUP;
		pp->pp_zlp = false;
	} else {
		pp->pp_stage = PUSBHC_STAGE_DATA;
		/* An OUT owes a closing zero-length packet when there
		 * is no data at all, or when the caller forces a short
		 * end on an exact multiple of the packet size. */
		pp->pp_zlp = !pusbhc_stage_isread(pp) &&
		    (xfer->ux_length == 0 ||
		    ((xfer->ux_flags & USBD_FORCE_SHORT_XFER) != 0 &&
		    xfer->ux_length % mps == 0));
	}
	xfer->ux_status = USBD_IN_PROGRESS;
	pusbhc_pipe_ready(sc, pp);
	mutex_exit(&sc->sc_intr_lock);

	usbd_xfer_schedule_timeout(xfer);

	return USBD_IN_PROGRESS;
}

static void
pusbhc_device_abort(struct usbd_xfer *xfer)
{
	struct pusbhc_softc *sc __diagused = PUSBHC_XFER2SC(xfer);

	KASSERT(mutex_owned(&sc->sc_lock));

	usbd_xfer_abort(xfer);
}

static void
pusbhc_device_close(struct usbd_pipe *pipe)
{
	struct pusbhc_pipe *pp __diagused = PUSBHC_PIPE2PP(pipe);

	/* The abort that precedes a close has emptied the pipe. */
	KASSERT(pp->pp_state == PUSBHC_PIPE_IDLE);
}

static void
pusbhc_device_cleartoggle(struct usbd_pipe *pipe)
{
	struct pusbhc_pipe *pp = PUSBHC_PIPE2PP(pipe);

	/* The endpoint restarts on DATA0 once its halt is cleared. */
	pp->pp_toggle = false;
}

static void
pusbhc_device_done(struct usbd_xfer *xfer)
{

	/* Engine bookkeeping was settled when the transfer left DONE. */
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

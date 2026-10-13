/*	$NetBSD$	*/

/*
 * Penumbra PS/2 port (CLASS_PS2), as a pckbport(9) controller.  The
 * port moves single bytes and translates no scan codes
 * (doc/system/devices/ps2.md); pckbd(4) above it does the rest.
 *
 * A byte nothing answers is sent again: a port may be a bridge that
 * acknowledges a byte and then drops it
 * (doc/internals/nexysvideo/overview.md, "Keyboard"), and pckbport
 * gives a command up at the first unanswered byte.
 *
 * The send does not wait for the answer itself.  pckbd polls for some
 * answers with the receive interrupt running, and an answer already
 * there when the send returned would reach the handler as a key.  The
 * polled read counts the wait instead, or a callout when nothing
 * polls.
 *
 * Only the keyboard slot is offered; a pointing device is not found.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/callout.h>
#include <sys/evcnt.h>
#include <sys/intr.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

#include <dev/pckbport/pckbportvar.h>
#include <dev/pckbport/pckbdreg.h>

/* Register map: doc/system/devices/ps2.md. */
#define	PKBC_CAP		0x00
#define	 PKBC_CAP_VERSION(x)	((u_int)__SHIFTOUT((x), __BITS(7, 0)))
#define	PKBC_IRQ_STATUS		0x04
#define	PKBC_IRQ_ENABLE		0x08
#define	 PKBC_IRQ_RX		__BIT(0)
#define	 PKBC_IRQ_TX		__BIT(1)
#define	PKBC_CTRL		0x0c
#define	 PKBC_CTRL_EN		__BIT(0)
#define	PKBC_RX_DATA		0x10
#define	 PKBC_RX_BYTE(x)	((int)__SHIFTOUT((x), __BITS(7, 0)))
#define	 PKBC_RX_VALID		__BIT(8)
#define	 PKBC_RX_ERR		__BIT(9)
#define	PKBC_TX_DATA		0x14

/* Longest the port spends on one byte. */
#define	PKBC_TX_MS		20

/*
 * Wait for an answer before a byte is sent again.  A device has 20 ms
 * to answer, and the port must be done with the byte (PKBC_TX_MS).
 */
#define	PKBC_ANSWER_MS		25
#define	PKBC_ANSWER_TICKS	(mstohz(PKBC_ANSWER_MS) + 1)

/*
 * Sends of one byte before giving up: enough for a bridge that drops
 * a quarter of its bytes, and over within the second pckbport allows
 * a command.
 */
#define	PKBC_SEND_TRIES		8

/* One polled read; pckbport counts its timeouts in these. */
#define	PKBC_POLL_MS		100

/* Bring-up drain: until quiet for PKBC_QUIET_MS, PKBC_DRAIN_MS at most. */
#define	PKBC_QUIET_MS		5
#define	PKBC_DRAIN_MS		500

/* Busy-wait step. */
#define	PKBC_STEP_US		100

struct pkbc_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	void			*sc_ih;
	pckbport_tag_t		sc_tag;

	/* Answer owed for the last byte sent; protected by spltty. */
	bool			sc_awaiting;	/* none arrived yet */
	u_char			sc_sent;	/* byte to resend */
	u_int			sc_tries;	/* resends left */
	u_int			sc_waited_us;	/* polled wait so far */
	struct callout		sc_answer_ch;	/* wait when not polling */

	struct evcnt		sc_ev_resend;
	struct evcnt		sc_ev_unanswered;
	struct evcnt		sc_ev_rxerr;
};

#define	PKBC_RD(sc, r)		\
	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define	PKBC_WR(sc, r, v)	\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))

static int	pkbc_match(device_t, cfdata_t, void *);
static void	pkbc_attach(device_t, device_t, void *);
static int	pkbc_intr(void *);
static void	pkbc_answer_timeout(void *);

CFATTACH_DECL_NEW(pkbc, sizeof(struct pkbc_softc),
    pkbc_match, pkbc_attach, NULL, NULL);

/* ── The link ─────────────────────────────────────────────────── */

/*
 * Write a byte once the port can take one; it ignores a write while
 * disabled or sending.  Returns false if the byte was not written.
 */
static bool
pkbc_tx(struct pkbc_softc *sc, u_char byte)
{
	u_int us;

	for (us = 0; (PKBC_RD(sc, PKBC_IRQ_STATUS) & PKBC_IRQ_TX) == 0;
	    us += PKBC_STEP_US) {
		if (us >= PKBC_TX_MS * 1000 ||
		    (PKBC_RD(sc, PKBC_CTRL) & PKBC_CTRL_EN) == 0)
			return false;
		DELAY(PKBC_STEP_US);
	}

	PKBC_WR(sc, PKBC_TX_DATA, byte);
	sc->sc_waited_us = 0;
	return true;
}

/*
 * Send a byte and expect an answer.  This replaces a byte still
 * unanswered, which pckbport has given up by then.
 */
static bool
pkbc_send(struct pkbc_softc *sc, u_char byte)
{

	callout_stop(&sc->sc_answer_ch);
	sc->sc_awaiting = false;

	if (!pkbc_tx(sc, byte))
		return false;

	sc->sc_sent = byte;
	sc->sc_tries = PKBC_SEND_TRIES - 1;
	sc->sc_awaiting = true;
	callout_schedule(&sc->sc_answer_ch, PKBC_ANSWER_TICKS);
	return true;
}

/*
 * No answer in time: send the byte again, or give up and leave
 * pckbport to time the command out.  Returns false when giving up.
 */
static bool
pkbc_resend(struct pkbc_softc *sc)
{

	if (sc->sc_tries == 0 || !pkbc_tx(sc, sc->sc_sent)) {
		sc->sc_awaiting = false;
		sc->sc_ev_unanswered.ev_count++;
		return false;
	}

	sc->sc_tries--;
	sc->sc_ev_resend.ev_count++;
	return true;
}

/*
 * Read a byte, or return -1 if there is none.  Any byte counts as the
 * answer owed: the port cannot tell an answer from a key, and if the
 * byte sent was lost after all, pckbport times the command out.
 */
static int
pkbc_rx(struct pkbc_softc *sc)
{
	uint32_t v;

	for (;;) {
		v = PKBC_RD(sc, PKBC_RX_DATA);
		if ((v & PKBC_RX_VALID) == 0)
			return -1;

		if (sc->sc_awaiting) {
			callout_stop(&sc->sc_answer_ch);
			sc->sc_awaiting = false;
		}

		if ((v & PKBC_RX_ERR) == 0)
			return PKBC_RX_BYTE(v);

		/* Damaged in transit; have the device send it again. */
		sc->sc_ev_rxerr.ev_count++;
		(void)pkbc_send(sc, KBC_RESEND);
	}
}

/*
 * Answer timeout when nothing polls.  A byte waiting unread is the
 * answer; the interrupt handler takes it after splx().
 */
static void
pkbc_answer_timeout(void *arg)
{
	struct pkbc_softc *sc = arg;
	int s;

	s = spltty();
	if (sc->sc_awaiting &&
	    (PKBC_RD(sc, PKBC_IRQ_STATUS) & PKBC_IRQ_RX) == 0 &&
	    pkbc_resend(sc))
		callout_schedule(&sc->sc_answer_ch, PKBC_ANSWER_TICKS);
	splx(s);
}

/*
 * Enable the port and discard what the device kept across the reset,
 * which pckbd would take for the answer to its first command.
 */
static void
pkbc_bringup(struct pkbc_softc *sc)
{
	u_int us, quiet_us;

	PKBC_WR(sc, PKBC_IRQ_ENABLE, 0);
	PKBC_WR(sc, PKBC_CTRL, PKBC_CTRL_EN);

	for (us = quiet_us = 0;
	    quiet_us < PKBC_QUIET_MS * 1000 && us < PKBC_DRAIN_MS * 1000;
	    us += PKBC_STEP_US) {
		if (PKBC_RD(sc, PKBC_RX_DATA) & PKBC_RX_VALID)
			quiet_us = 0;
		else
			quiet_us += PKBC_STEP_US;
		DELAY(PKBC_STEP_US);
	}
}

/* ── pckbport(9) controller operations ────────────────────────── */

/* No translation here: -1 has pckbd convert scan code set 2 itself. */
static int
pkbc_xt_translation(void *cookie, pckbport_slot_t slot, int on)
{

	return -1;
}

static int
pkbc_send_devcmd(void *cookie, pckbport_slot_t slot, u_char byte)
{
	struct pkbc_softc *sc = cookie;
	bool sent;
	int s;

	s = spltty();
	sent = pkbc_send(sc, byte);
	splx(s);

	return sent;
}

/*
 * Wait up to PKBC_POLL_MS for a byte.  spltty keeps the interrupt
 * handler from taking it first, and the callout from running, so the
 * answer wait is counted here.
 */
static int
pkbc_poll_data1(void *cookie, pckbport_slot_t slot)
{
	struct pkbc_softc *sc = cookie;
	u_int us;
	int c, s;

	s = spltty();
	callout_stop(&sc->sc_answer_ch);

	for (us = 0; (c = pkbc_rx(sc)) == -1 && us < PKBC_POLL_MS * 1000;
	    us += PKBC_STEP_US) {
		DELAY(PKBC_STEP_US);
		if (!sc->sc_awaiting)
			continue;
		sc->sc_waited_us += PKBC_STEP_US;
		if (sc->sc_waited_us >= PKBC_ANSWER_MS * 1000)
			(void)pkbc_resend(sc);
	}

	if (sc->sc_awaiting)
		callout_schedule(&sc->sc_answer_ch, PKBC_ANSWER_TICKS);
	splx(s);

	return c;
}

/*
 * The slot is the port.  A disabled port drops the byte it held and
 * the one it was sending, so no answer is owed.
 */
static void
pkbc_slot_enable(void *cookie, pckbport_slot_t slot, int on)
{
	struct pkbc_softc *sc = cookie;
	int s;

	s = spltty();
	if (!on) {
		callout_stop(&sc->sc_answer_ch);
		sc->sc_awaiting = false;
	}
	PKBC_WR(sc, PKBC_CTRL, on ? PKBC_CTRL_EN : 0);
	splx(s);
}

static void
pkbc_intr_establish(void *cookie, pckbport_slot_t slot)
{
	struct pkbc_softc *sc = cookie;

	if (sc->sc_ih != NULL)
		return;

	sc->sc_ih = intr_establish_xname(0, IPL_TTY, pkbc_intr, sc,
	    device_xname(sc->sc_dev));
	if (sc->sc_ih == NULL) {
		aprint_error_dev(sc->sc_dev, "can't establish interrupt\n");
		return;
	}
	PKBC_WR(sc, PKBC_IRQ_ENABLE, PKBC_IRQ_RX);
}

/* While pckbport polls, received bytes are its to fetch. */
static void
pkbc_set_poll(void *cookie, pckbport_slot_t slot, int on)
{
	struct pkbc_softc *sc = cookie;

	PKBC_WR(sc, PKBC_IRQ_ENABLE,
	    (on || sc->sc_ih == NULL) ? 0 : PKBC_IRQ_RX);
}

static const struct pckbport_accessops pkbc_ops = {
	.t_xt_translation = pkbc_xt_translation,
	.t_send_devcmd = pkbc_send_devcmd,
	.t_poll_data1 = pkbc_poll_data1,
	.t_slot_enable = pkbc_slot_enable,
	.t_intr_establish = pkbc_intr_establish,
	.t_set_poll = pkbc_set_poll,
};

/* ── Interrupt ────────────────────────────────────────────────── */

static int
pkbc_intr(void *arg)
{
	struct pkbc_softc *sc = arg;
	int c;

	/* Shared line: ours only if RX is pending and enabled. */
	if ((PKBC_RD(sc, PKBC_IRQ_STATUS) & PKBC_IRQ_RX) == 0 ||
	    (PKBC_RD(sc, PKBC_IRQ_ENABLE) & PKBC_IRQ_RX) == 0)
		return 0;

	while ((c = pkbc_rx(sc)) != -1)
		pckbportintr(sc->sc_tag, PCKBPORT_KBD_SLOT, c);

	return 1;
}

/* ── Autoconfiguration ────────────────────────────────────────── */

static int
pkbc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_PS2;
}

static void
pkbc_attach(device_t parent, device_t self, void *aux)
{
	struct pkbc_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	uint32_t cap;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map PS/2 port registers: %d\n", error);
		return;
	}

	cap = PKBC_RD(sc, PKBC_CAP);

	aprint_naive("\n");
	aprint_normal(": Penumbra PS/2 port (v%u)\n", PKBC_CAP_VERSION(cap));

	if (PKBC_CAP_VERSION(cap) != 1) {
		aprint_error_dev(self, "unsupported version\n");
		return;
	}

	callout_init(&sc->sc_answer_ch, 0);
	callout_setfunc(&sc->sc_answer_ch, pkbc_answer_timeout, sc);

	evcnt_attach_dynamic(&sc->sc_ev_resend, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "bytes sent again");
	evcnt_attach_dynamic(&sc->sc_ev_unanswered, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "bytes never answered");
	evcnt_attach_dynamic(&sc->sc_ev_rxerr, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "receive errors");

	pkbc_bringup(sc);

	/*
	 * pckbd probes by resetting the keyboard now.  Without one the
	 * port is disabled again, and a keyboard attached later is not
	 * seen.
	 */
	sc->sc_tag = pckbport_attach(sc, &pkbc_ops);
	if (pckbport_attach_slot(self, sc->sc_tag, PCKBPORT_KBD_SLOT) == NULL) {
		aprint_normal_dev(self, "no keyboard found\n");
		PKBC_WR(sc, PKBC_CTRL, 0);
	}
}

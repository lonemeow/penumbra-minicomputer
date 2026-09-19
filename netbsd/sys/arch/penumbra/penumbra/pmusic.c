/*	$NetBSD$	*/

/*
 * /dev/pmusic: the Penumbra audio device's channels as voices.
 *
 * The device opens exclusively, and the open is a session: voices
 * claimed from the device, a sample memory written with pwrite(2), and
 * a queue of events timed in ticks of a clock the kernel keeps.  The
 * consumer runs ahead of the clock, the clock applies each tick's
 * events in one pass, and the device's watermark interrupt refills
 * every sounding voice's channel from sample memory.  The interface is
 * <machine/penaudio.h>; the reasons for its shape are in
 * doc/system/netbsd/design-notes.md, "Audio Driver".
 *
 * Locking: sc_pm_lock serializes the session's calls, which copy to
 * and from user space.  Everything the clock callout and the interrupt
 * touch — voices, queue, clock, and the device registers — is under
 * sc_intr_lock, which the clock takes from softclock and the refill
 * holds from the interrupt.  sc_pm_klock covers only the knote list,
 * so the clock can wake a poller without waiting on a session call
 * that is copying from user space.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitops.h>
#include <sys/callout.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/event.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <arch/penumbra/penumbra/paudiovar.h>

#include "ioconf.h"

/* Event queue capacity: a power of two, so ring slots wrap by mask. */
#define	PM_MAXEVENTS		512
#define	PM_QSLOT(i)		((i) & (PM_MAXEVENTS - 1))

#define	PM_SET_VOICE		(PENAUDIO_SET_INC | PENAUDIO_SET_VOL | \
				 PENAUDIO_SET_PLAY | PENAUDIO_SET_LOOP | \
				 PENAUDIO_SET_STOP)
#define	PM_SET_ALL		(PENAUDIO_SET_PERIOD | PM_SET_VOICE)

/* The default sample memory cap is this fraction of physical memory,
 * rounded up to a power of two. */
#define	PM_MAXMEM_SHARE		64

struct pmusic_pass {
	uint32_t	pp_start;	/* channels restarting from a new pass */
	uint32_t	pp_stop;	/* channels falling silent */
	uint32_t	pp_relatch;	/* channels whose loop changed */
};

/* Where the device is in a voice's data. */
struct pmusic_spot {
	struct pmusic_span sp_pass;	/* the pass it is playing */
	u_int		sp_frame;	/* the next frame it plays there */
};

static dev_type_open(pmusic_open);
static dev_type_close(pmusic_close);
static dev_type_read(pmusic_read);
static dev_type_write(pmusic_write);
static dev_type_ioctl(pmusic_ioctl);
static dev_type_poll(pmusic_poll);
static dev_type_kqfilter(pmusic_kqfilter);

const struct cdevsw pmusic_cdevsw = {
	.d_open		= pmusic_open,
	.d_close	= pmusic_close,
	.d_read		= pmusic_read,
	.d_write	= pmusic_write,
	.d_ioctl	= pmusic_ioctl,
	.d_stop		= nostop,
	.d_tty		= notty,
	.d_poll		= pmusic_poll,
	.d_mmap		= nommap,
	.d_kqfilter	= pmusic_kqfilter,
	.d_discard	= nodiscard,
	.d_flag		= D_OTHER | D_MPSAFE,
};

static void	pmusic_clock(void *);

static struct paudio_softc *
pmusic_lookup(dev_t dev)
{
	struct paudio_softc *sc;

	sc = device_lookup_private(&paudio_cd, minor(dev));
	return (sc != NULL && sc->sc_pm_ready) ? sc : NULL;
}

/* log2 of a format's frame size in bytes, or -1 for no format. */
static int
pmusic_frame_shift(uint32_t format)
{
	switch (format) {
	case PENAUDIO_FMT_S8:		return 0;
	case PENAUDIO_FMT_S16:		return 1;
	case PENAUDIO_FMT_S16_STEREO:	return 2;
	default:			return -1;
	}
}

static void
pmusic_span_set(struct pmusic_span *sn, const struct penaudio_region *pr,
    uint32_t format)
{
	sn->sn_base = pr->pr_offset;
	sn->sn_frames = pr->pr_length >> pmusic_frame_shift(format);
	sn->sn_format = format;
}

static bool
pmusic_span_eq(const struct pmusic_span *a, const struct pmusic_span *b)
{
	return a->sn_base == b->sn_base && a->sn_frames == b->sn_frames &&
	    a->sn_format == b->sn_format;
}

/* Where a span's frame starts in sample memory. */
static const uint8_t *
pmusic_frame_addr(const struct paudio_softc *sc,
    const struct pmusic_span *sn, u_int frame)
{
	return sc->sc_pm_mem + sn->sn_base +
	    (frame << pmusic_frame_shift(sn->sn_format));
}

/* ── Refill ───────────────────────────────────────────────────── */

/*
 * Move the refill from the end of its pass to the start of the loop,
 * keeping the account pmusic_locate() reads of what is queued behind
 * it.
 */
static void
pmusic_wrap(struct pmusic_voice *pv)
{
	if (pv->pv_head.sn_frames == 0) {
		pv->pv_head = pv->pv_pass;
		pv->pv_looped = 0;
	} else {
		pv->pv_looped = MIN(pv->pv_looped + pv->pv_pass.sn_frames,
		    pv->pv_depth);
	}
	pv->pv_pass = pv->pv_loop;
	pv->pv_at = 0;
}

/*
 * Find where the device is in a voice's data, given the frames still
 * queued on its channel.  The oldest frame queued is the next one the
 * device plays, and the queue holds the last `queued` frames of: the
 * head, then pv_looped frames of copies of the refill's pass, then the
 * first pv_at frames of the refill's pass.
 *
 * Returns false when that frame is in the refill's pass — the refill
 * has not queued past the end of the pass the device is playing.
 * Otherwise fills *sp and returns true.
 */
static bool
pmusic_locate(const struct pmusic_voice *pv, u_int queued,
    struct pmusic_spot *sp)
{
	u_int back, len;

	if (queued <= pv->pv_at)
		return false;

	/* Frames before the start of the refill's pass. */
	back = queued - pv->pv_at;

	if (back <= pv->pv_looped) {
		/* In a copy: counting back from a copy's end, frame
		 * len - 1 is 1 back and frame 0 is len back. */
		len = pv->pv_pass.sn_frames;
		sp->sp_pass = pv->pv_pass;
		sp->sp_frame = len - 1 - (back - 1) % len;
	} else {
		back -= pv->pv_looped;
		KASSERT(back <= pv->pv_head.sn_frames);
		sp->sp_pass = pv->pv_head;
		sp->sp_frame = pv->pv_head.sn_frames - back;
	}
	return true;
}

/*
 * Write up to n frames of a voice's data into its channel, following
 * the pass into the loop.  Every format becomes the channel's native
 * 16-bit stereo, a mono sample on both sides, so the voice's VOL is
 * what places it.  Sets pv_draining when the data runs out.
 * Called with sc_intr_lock held.
 */
static void
pmusic_push(struct paudio_softc *sc, struct pmusic_voice *pv, u_int n)
{
	bus_size_t port = PA_CHAN(pv->pv_chan) + PA_C_FIFO;

	while (n > 0) {
		const uint8_t *src;
		u_int k, i;

		if (pv->pv_at == pv->pv_pass.sn_frames) {
			if (pv->pv_loop.sn_frames == 0) {
				pv->pv_draining = true;
				return;
			}
			pmusic_wrap(pv);
		}

		k = MIN(n, pv->pv_pass.sn_frames - pv->pv_at);
		src = pmusic_frame_addr(sc, &pv->pv_pass, pv->pv_at);

		switch (pv->pv_pass.sn_format) {
		case PENAUDIO_FMT_S8:
			/* A signed byte shifted into the high half is the
			 * same two's complement value at 16 bits. */
			for (i = 0; i < k; i++) {
				uint32_t s = (uint32_t)src[i] << 8;

				PA_WR(sc, port, s | (s << 16));
			}
			break;
		case PENAUDIO_FMT_S16:
			for (i = 0; i < k; i++) {
				uint32_t s = ((const uint16_t *)src)[i];

				PA_WR(sc, port, s | (s << 16));
			}
			break;
		default:
			bus_space_write_multi_4(sc->sc_iot, sc->sc_ioh, port,
			    (const uint32_t *)src, k);
			break;
		}

		pv->pv_at += k;
		n -= k;
	}
}

/*
 * Take channels out of the mix and discard what they hold.  They are
 * disabled before the flush, so none runs dry on the way out and flags
 * an underrun.  Writing FLUSH alone also clears the format bits, which
 * this driver never sets.  Called with sc_intr_lock held.
 */
static void
pmusic_quiet(struct paudio_softc *sc, uint32_t chans)
{
	uint32_t left = chans;

	paudio_chan_update(sc, PA_CHAN_EN, chans, false);
	paudio_chan_update(sc, PA_IRQ_ENABLE, chans, false);

	while (left != 0) {
		u_int ch = ffs32(left) - 1;

		left &= ~PA_CHBIT(ch);
		PA_CHWR(sc, ch, PA_C_CTRL, PA_C_CTRL_FLUSH);
		PA_CHWR(sc, ch, PA_C_WM, 0);
	}
	PA_WR(sc, PA_UNDERRUN, chans);
	PA_WR(sc, PA_IRQ_STATUS, chans);
}

/*
 * Keep a sounding voice's channel full, and aim its watermark at the
 * point where half remains.  Once the data has run out, aim it at the
 * empty queue instead, and silence the voice when that arrives — a
 * sound that ends runs its channel dry, which is not an underrun.
 *
 * A full queue costs nothing in how soon a change is heard: rate and
 * gain act on the frame being played, a new sound flushes, and a new
 * loop relatches.  Only an edit to sample data already queued waits
 * for it to play out.  Called with sc_intr_lock held.
 */
static void
pmusic_refill(struct paudio_softc *sc, struct pmusic_voice *pv,
    uint32_t underrun)
{
	uint32_t bit = PA_CHBIT(pv->pv_chan);
	u_int queued;

	if (!pv->pv_sounding)
		return;

	queued = pv->pv_depth - PA_CHRD(sc, pv->pv_chan, PA_C_LEVEL);

	if (pv->pv_draining) {
		if (queued == 0) {
			pmusic_quiet(sc, bit);
			pv->pv_sounding = false;
			pv->pv_draining = false;
		}
		return;
	}

	if (underrun & bit) {
		sc->sc_pm_underruns++;
		PA_WR(sc, PA_UNDERRUN, bit);
	}

	if (queued < pv->pv_depth)
		pmusic_push(sc, pv, pv->pv_depth - queued);

	PA_CHWR(sc, pv->pv_chan, PA_C_WM, pv->pv_draining ? pv->pv_depth :
	    pv->pv_depth - pv->pv_depth / 2);
}

/*
 * Make a changed loop follow the pass the device is playing, rather
 * than the one the refill has reached.  When the refill has already
 * queued past the end of the device's pass, what the queue holds after
 * it is the old loop: discard the queue and fill it again from the
 * device's position.  FLUSH also clears the channel's phase, which
 * costs less than one source frame.  Called with sc_intr_lock held.
 */
static void
pmusic_relatch(struct paudio_softc *sc, struct pmusic_voice *pv)
{
	struct pmusic_spot sp;
	uint32_t bit = PA_CHBIT(pv->pv_chan);
	u_int queued;

	if (PA_RD(sc, PA_UNDERRUN) & bit) {
		sc->sc_pm_underruns++;
		PA_WR(sc, PA_UNDERRUN, bit);
	}

	queued = pv->pv_depth - PA_CHRD(sc, pv->pv_chan, PA_C_LEVEL);
	if (pmusic_locate(pv, queued, &sp)) {
		/* Straight after the LEVEL read: any frame the device
		 * takes between the two is heard twice. */
		PA_CHWR(sc, pv->pv_chan, PA_C_CTRL, PA_C_CTRL_FLUSH);

		pv->pv_pass = sp.sp_pass;
		pv->pv_at = sp.sp_frame;
		pmusic_push(sc, pv, pv->pv_depth);

		/* Dry from the flush to the first frame: ours, not an
		 * underrun. */
		PA_WR(sc, PA_UNDERRUN, bit);
	}

	/* The device is in the refill's pass now either way, so nothing
	 * queued behind it is needed again. */
	pv->pv_head.sn_frames = 0;
	pv->pv_looped = 0;
}

/*
 * Refill every voice.  One voice reaching its watermark tops up all of
 * them, so a voice that would come due soon after rides along on this
 * interrupt instead of raising its own.  Called with sc_intr_lock held.
 */
static void
pmusic_topup(struct paudio_softc *sc)
{
	uint32_t underrun = PA_RD(sc, PA_UNDERRUN) & sc->sc_pm_chans;
	u_int v;

	for (v = 0; v < sc->sc_pm_nvoices; v++)
		pmusic_refill(sc, &sc->sc_pm_voice[v], underrun);
}

int
pmusic_intr(struct paudio_softc *sc, uint32_t status)
{
	KASSERT(mutex_owned(&sc->sc_intr_lock));

	status &= sc->sc_pm_chans;
	if (status == 0)
		return 0;
	sc->sc_ev_pm_intr.ev_count++;

	pmusic_topup(sc);

	/* Clear last: the flag is level-sensitive on free space. */
	PA_WR(sc, PA_IRQ_STATUS, status);
	return 1;
}

/* ── Events ───────────────────────────────────────────────────── */

static bool
pmusic_region_ok(const struct paudio_softc *sc,
    const struct penaudio_region *pr, int shift)
{
	uint32_t align = (1U << shift) - 1;

	if (pr->pr_length == 0)
		return true;
	if (((pr->pr_offset | pr->pr_length) & align) != 0)
		return false;
	return pr->pr_length <= sc->sc_pm_memsize &&
	    pr->pr_offset <= sc->sc_pm_memsize - pr->pr_length;
}

/*
 * Whether one event is acceptable on its own.  Where it falls against
 * the queue depends on the clock, and is checked when it is committed.
 */
static bool
pmusic_event_ok(const struct paudio_softc *sc,
    const struct penaudio_event *pe)
{
	u_int set = pe->pe_set;
	int shift;

	if ((set & ~PM_SET_ALL) != 0)
		return false;
	if (set & PENAUDIO_SET_PERIOD)
		return set == PENAUDIO_SET_PERIOD &&
		    pe->pe_voice == PENAUDIO_VOICE_NONE &&
		    pe->pe_period_us >= PENAUDIO_PERIOD_MIN_US;
	if (set == 0)
		return true;

	if (pe->pe_voice >= sc->sc_pm_nvoices)
		return false;
	if ((set & PENAUDIO_SET_STOP) &&
	    (set & (PENAUDIO_SET_PLAY | PENAUDIO_SET_LOOP)))
		return false;
	if ((set & PENAUDIO_SET_INC) &&
	    (pe->pe_inc < PENAUDIO_INC_MIN || pe->pe_inc > PENAUDIO_INC_MAX))
		return false;
	if (set & (PENAUDIO_SET_PLAY | PENAUDIO_SET_LOOP)) {
		shift = pmusic_frame_shift(pe->pe_format);
		if (shift < 0 || !pmusic_region_ok(sc, &pe->pe_loop, shift))
			return false;
		if ((set & PENAUDIO_SET_PLAY) &&
		    !pmusic_region_ok(sc, &pe->pe_play, shift))
			return false;
	}
	return true;
}

static void
pmusic_set_period(struct paudio_softc *sc, uint32_t period_us)
{
	sc->sc_pm_period_hz = period_us / tick;
	sc->sc_pm_period_us = period_us % tick;
}

/*
 * Apply one event within a pass.  Rate and gain go to the channel at
 * once; what a channel plays is only recorded, and pmusic_pass_end()
 * acts on it for the whole pass together.  Called with sc_intr_lock
 * held.
 */
static void
pmusic_pass_event(struct paudio_softc *sc, struct pmusic_pass *pp,
    const struct penaudio_event *pe)
{
	struct pmusic_voice *pv;
	struct pmusic_span loop;
	uint32_t bit;
	u_int set = pe->pe_set;

	if (set & PENAUDIO_SET_PERIOD) {
		pmusic_set_period(sc, pe->pe_period_us);
		return;
	}
	if (set == 0)
		return;

	pv = &sc->sc_pm_voice[pe->pe_voice];
	bit = PA_CHBIT(pv->pv_chan);

	if (set & PENAUDIO_SET_INC)
		PA_CHWR(sc, pv->pv_chan, PA_C_INC, pe->pe_inc);
	if (set & PENAUDIO_SET_VOL)
		PA_CHWR(sc, pv->pv_chan, PA_C_VOL,
		    (uint32_t)pe->pe_vol_r << 16 | pe->pe_vol_l);
	if (set & PENAUDIO_SET_PLAY) {
		pmusic_span_set(&pv->pv_pass, &pe->pe_play, pe->pe_format);
		pv->pv_at = 0;
		pv->pv_head.sn_frames = 0;
		pv->pv_looped = 0;
	}
	if (set & (PENAUDIO_SET_PLAY | PENAUDIO_SET_LOOP)) {
		pmusic_span_set(&loop, &pe->pe_loop, pe->pe_format);
		if ((set & PENAUDIO_SET_LOOP) &&
		    !pmusic_span_eq(&loop, &pv->pv_loop)) {
			pp->pp_relatch |= bit;
			if (pv->pv_draining) {
				/* It had queued all it had, and the loop
				 * continues it instead; a channel that ran
				 * dry meanwhile was the sound ending, not an
				 * underrun. */
				pv->pv_draining = false;
				PA_WR(sc, PA_UNDERRUN, bit);
			}
		}
		pv->pv_loop = loop;
	}
	if ((set & PENAUDIO_SET_PLAY) &&
	    (pe->pe_play.pr_length != 0 || pe->pe_loop.pr_length != 0)) {
		pp->pp_start |= bit;
		pp->pp_stop &= ~bit;
	} else if (set & (PENAUDIO_SET_PLAY | PENAUDIO_SET_STOP)) {
		pp->pp_stop |= bit;
		pp->pp_start &= ~bit;
	}
}

/*
 * Finish a pass: restart, silence and relatch the channels its events
 * named.  Every channel changing is stopped in one write and the
 * restarting ones started in another, so their onsets coincide however
 * long priming them takes in between.  A channel starting over needs
 * no relatch; it begins its new pass from empty.  Called with
 * sc_intr_lock held.
 */
static void
pmusic_pass_end(struct paudio_softc *sc, const struct pmusic_pass *pp)
{
	uint32_t changing = pp->pp_start | pp->pp_stop;
	u_int v;

	if (changing != 0)
		pmusic_quiet(sc, changing);

	for (v = 0; v < sc->sc_pm_nvoices; v++) {
		struct pmusic_voice *pv = &sc->sc_pm_voice[v];
		uint32_t bit = PA_CHBIT(pv->pv_chan);

		if (changing & bit) {
			pv->pv_sounding = (pp->pp_start & bit) != 0;
			pv->pv_draining = false;
			pmusic_refill(sc, pv, 0);
		} else if ((pp->pp_relatch & bit) && pv->pv_sounding) {
			pmusic_relatch(sc, pv);
		}
	}

	if (changing == 0)
		return;
	if (!sc->sc_pm_paused)
		paudio_chan_update(sc, PA_CHAN_EN, pp->pp_start, true);
	paudio_chan_update(sc, PA_IRQ_ENABLE, pp->pp_start, true);
}

/* ── Tick clock ───────────────────────────────────────────────── */

/* The clock tick the next tick applies on: the first at or after it
 * falls due. */
static u_int
pmusic_due(const struct paudio_softc *sc)
{
	return sc->sc_pm_due_hz + (sc->sc_pm_due_us != 0);
}

static void
pmusic_due_advance(struct paudio_softc *sc)
{
	sc->sc_pm_due_hz += sc->sc_pm_period_hz;
	sc->sc_pm_due_us += sc->sc_pm_period_us;
	if (sc->sc_pm_due_us >= (uint32_t)tick) {
		sc->sc_pm_due_us -= tick;
		sc->sc_pm_due_hz++;
	}
}

/*
 * Advance one tick and apply its events in one pass, then time the
 * next from this one's due time, at the period those events left.
 * Called with sc_intr_lock held.
 */
static void
pmusic_advance(struct paudio_softc *sc)
{
	struct pmusic_pass pp = { 0, 0, 0 };

	sc->sc_pm_tick++;
	while (sc->sc_pm_qcount > 0) {
		const struct penaudio_event *pe = &sc->sc_pm_q[sc->sc_pm_qhead];

		if (pe->pe_tick > sc->sc_pm_tick)
			break;
		pmusic_pass_event(sc, &pp, pe);
		sc->sc_pm_qhead = PM_QSLOT(sc->sc_pm_qhead + 1);
		sc->sc_pm_qcount--;
	}
	pmusic_pass_end(sc, &pp);
	pmusic_due_advance(sc);
}

/*
 * Whether a consumer waiting to submit should wake: the queue has
 * room, and the clock either is not running or has come within
 * pc_lowat ticks of the horizon.  Called with sc_intr_lock held.
 */
static bool
pmusic_writable(const struct paudio_softc *sc)
{
	if (sc->sc_pm_qcount == PM_MAXEVENTS)
		return false;
	return !sc->sc_pm_started ||
	    sc->sc_pm_through - sc->sc_pm_tick < sc->sc_pm_lowat;
}

static void
pmusic_notify(struct paudio_softc *sc)
{
	mutex_enter(&sc->sc_pm_klock);
	selnotify(&sc->sc_pm_wsel, POLLOUT | POLLWRNORM, NOTE_SUBMIT);
	mutex_exit(&sc->sc_pm_klock);
}

/*
 * The clock callout: apply every tick now due, unless the next is past
 * the horizon, in which case wait there — unscheduled, until a SUBMIT
 * declares it — rather than letting several ticks' effects collapse
 * into one instant when the consumer catches up.
 */
static void
pmusic_clock(void *arg)
{
	struct paudio_softc *sc = arg;
	u_int now, due;
	bool notify;

	mutex_spin_enter(&sc->sc_intr_lock);
	if (!sc->sc_pm_started || sc->sc_pm_paused || sc->sc_pm_stalled) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return;
	}

	now = getticks();
	while ((int)((due = pmusic_due(sc)) - now) <= 0) {
		if (sc->sc_pm_tick == sc->sc_pm_through) {
			sc->sc_pm_stalled = true;
			sc->sc_pm_stall_at = due;
			break;
		}
		pmusic_advance(sc);
	}
	if (!sc->sc_pm_stalled)
		callout_schedule(&sc->sc_pm_clock, due - now);

	pmusic_topup(sc);
	notify = pmusic_writable(sc);
	mutex_spin_exit(&sc->sc_intr_lock);

	if (notify)
		pmusic_notify(sc);
}

/*
 * Resume a stalled clock once the horizon has moved past its tick.
 * The stalled tick applies on the next clock tick and the rest of the
 * timeline shifts by the wait.  A paused clock resumes from the moment
 * it paused, since that is what un-pausing shifts it by.
 * Called with sc_intr_lock held.
 */
static void
pmusic_unstall(struct paudio_softc *sc)
{
	u_int at;

	if (!sc->sc_pm_stalled || sc->sc_pm_through == sc->sc_pm_tick)
		return;

	at = sc->sc_pm_paused ? sc->sc_pm_paused_at : (u_int)getticks();
	sc->sc_pm_stalls += at + 1 - sc->sc_pm_stall_at;
	sc->sc_pm_due_hz = at + 1;
	sc->sc_pm_due_us = 0;
	sc->sc_pm_stalled = false;
	if (!sc->sc_pm_paused)
		callout_schedule(&sc->sc_pm_clock, 1);
}

/* ── Session calls ────────────────────────────────────────────── */

/* Whether the session has anything playing or pending, which fixes
 * its allocation and its sample memory.  Called with sc_intr_lock
 * held. */
static bool
pmusic_busy(const struct paudio_softc *sc)
{
	u_int v;

	if (sc->sc_pm_qcount > 0)
		return true;
	for (v = 0; v < sc->sc_pm_nvoices; v++)
		if (sc->sc_pm_voice[v].pv_sounding)
			return true;
	return false;
}

/* Channels a voice allocation could take now.  Called with
 * sc_intr_lock held. */
static uint32_t
pmusic_free_chans(const struct paudio_softc *sc)
{
	uint32_t chans;

	if (sc->sc_pm_nvoices_max == 0)
		return 0;
	chans = sc->sc_pm_nvoices_max == PA_MAXCHAN ? ~0U :
	    PA_CHBIT(sc->sc_pm_nvoices_max) - 1;
	if (sc->sc_audio_open)
		chans &= ~__BIT(PA_CHAN0);
	return chans;
}

/*
 * Silence and release every voice.  Rate and gain go back to unity,
 * as audio(4) plays channel 0 without setting either.
 * Called with sc_intr_lock held.
 */
static void
pmusic_release(struct paudio_softc *sc)
{
	uint32_t left = sc->sc_pm_chans;

	pmusic_quiet(sc, sc->sc_pm_chans);
	while (left != 0) {
		u_int ch = ffs32(left) - 1;

		left &= ~PA_CHBIT(ch);
		PA_CHWR(sc, ch, PA_C_INC, PA_INC_UNITY);
		PA_CHWR(sc, ch, PA_C_VOL, PA_VOL_UNITY);
	}
	memset(sc->sc_pm_voice, 0, sizeof(sc->sc_pm_voice));
	sc->sc_pm_nvoices = 0;
	sc->sc_pm_chans = 0;
	paudio_output_update(sc);
}

static void
pmusic_status(struct paudio_softc *sc, struct penaudio_status *st)
{
	u_int v;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	memset(st, 0, sizeof(*st));
	st->pst_tick = sc->sc_pm_tick;
	st->pst_through = sc->sc_pm_through;
	st->pst_room = PM_MAXEVENTS - sc->sc_pm_qcount;
	st->pst_stalls = sc->sc_pm_stalls;
	if (sc->sc_pm_stalled)
		st->pst_stalls += (sc->sc_pm_paused ? sc->sc_pm_paused_at :
		    (u_int)getticks()) - sc->sc_pm_stall_at;
	st->pst_underruns = sc->sc_pm_underruns;
	for (v = 0; v < sc->sc_pm_nvoices; v++)
		if (sc->sc_pm_voice[v].pv_sounding)
			st->pst_sounding |= 1U << v;
}

static void
pmusic_getinfo(struct paudio_softc *sc, struct penaudio_info *pi)
{
	memset(pi, 0, sizeof(*pi));
	pi->pi_rate = sc->sc_rate;
	pi->pi_nvoices = sc->sc_pm_nvoices_max;
	mutex_spin_enter(&sc->sc_intr_lock);
	pi->pi_navail = popcount32(pmusic_free_chans(sc));
	mutex_spin_exit(&sc->sc_intr_lock);
	pi->pi_maxmem = sc->sc_pm_maxmem;
	pi->pi_maxevents = PM_MAXEVENTS;
	pi->pi_tick_grain_us = tick;
}

static int
pmusic_alloc(struct paudio_softc *sc, uint32_t *nvoicesp)
{
	uint32_t free;
	u_int want = *nvoicesp, v = 0, ch;

	mutex_spin_enter(&sc->sc_intr_lock);
	if (pmusic_busy(sc)) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return EBUSY;
	}
	pmusic_release(sc);

	/* From the top down, so channel 0, the only one a stream can
	 * use, is the last to go. */
	free = pmusic_free_chans(sc);
	for (ch = sc->sc_pm_nvoices_max; ch-- > 0 && v < want;) {
		struct pmusic_voice *pv = &sc->sc_pm_voice[v];

		if ((free & PA_CHBIT(ch)) == 0)
			continue;
		pv->pv_chan = ch;
		pv->pv_depth = PA_CHRD(sc, ch, PA_C_DEPTH);
		PA_CHWR(sc, ch, PA_C_INC, PA_INC_UNITY);
		PA_CHWR(sc, ch, PA_C_VOL, PA_VOL_UNITY);
		sc->sc_pm_chans |= PA_CHBIT(ch);
		v++;
	}
	sc->sc_pm_nvoices = v;

	pmusic_quiet(sc, sc->sc_pm_chans);
	paudio_output_update(sc);
	mutex_spin_exit(&sc->sc_intr_lock);

	*nvoicesp = v;
	return 0;
}

static int
pmusic_setmem(struct paudio_softc *sc, uint32_t size)
{
	uint8_t *mem = NULL, *old;
	uint32_t oldsize;

	if (size > (uint32_t)sc->sc_pm_maxmem)
		return EINVAL;

	/* Nothing can start playing while this call holds sc_pm_lock,
	 * so the answer holds until the swap. */
	mutex_spin_enter(&sc->sc_intr_lock);
	if (pmusic_busy(sc)) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return EBUSY;
	}
	mutex_spin_exit(&sc->sc_intr_lock);

	if (size > 0)
		mem = kmem_zalloc(size, KM_SLEEP);

	mutex_spin_enter(&sc->sc_intr_lock);
	old = sc->sc_pm_mem;
	oldsize = sc->sc_pm_memsize;
	sc->sc_pm_mem = mem;
	sc->sc_pm_memsize = size;
	mutex_spin_exit(&sc->sc_intr_lock);

	if (old != NULL)
		kmem_free(old, oldsize);
	return 0;
}

/*
 * Copy offered events into the queue's free slots, which nothing reads
 * until they are committed: the clock only consumes from the head, and
 * only a SUBMIT, serialized by sc_pm_lock, writes past the tail.
 */
static int
pmusic_stage(struct paudio_softc *sc, const struct penaudio_event *uev,
    u_int first, u_int n)
{
	u_int run = MIN(n, PM_MAXEVENTS - first);
	int error;

	error = copyin(uev, &sc->sc_pm_q[first], run * sizeof(*uev));
	if (error == 0 && run < n)
		error = copyin(uev + run, &sc->sc_pm_q[0],
		    (n - run) * sizeof(*uev));
	return error;
}

static int
pmusic_submit(struct paudio_softc *sc, struct penaudio_submit *ps)
{
	struct pmusic_pass pp = { 0, 0, 0 };
	u_int first, staged, valid, i, queued;
	uint32_t last, refused_tick = 0, through;
	bool refused;
	int error;

	if (ps->ps_eventsize != sizeof(struct penaudio_event))
		return EINVAL;

	/* The tail stays put while the clock consumes: the head and the
	 * count move together. */
	mutex_spin_enter(&sc->sc_intr_lock);
	first = PM_QSLOT(sc->sc_pm_qhead + sc->sc_pm_qcount);
	staged = MIN(ps->ps_nevents, PM_MAXEVENTS - sc->sc_pm_qcount);
	mutex_spin_exit(&sc->sc_intr_lock);

	error = pmusic_stage(sc, ps->ps_events, first, staged);
	if (error)
		return error;

	/* The first event that does not fit bounds the horizon below. */
	if (staged < ps->ps_nevents) {
		error = copyin(&ps->ps_events[staged].pe_tick, &refused_tick,
		    sizeof(refused_tick));
		if (error)
			return error;
	}

	for (valid = 0; valid < staged; valid++)
		if (!pmusic_event_ok(sc, &sc->sc_pm_q[PM_QSLOT(first + valid)]))
			break;

	/*
	 * Commit.  An event whose tick has already been applied joins
	 * this call's pass; the rest are queued behind the tail, and none
	 * may precede what is queued already.  Queued events move down
	 * over the slots of applied ones, so the slot read is never one
	 * already written.
	 */
	mutex_spin_enter(&sc->sc_intr_lock);
	last = sc->sc_pm_qcount > 0 ? sc->sc_pm_qlast : 0;
	queued = 0;
	for (i = 0; i < valid; i++) {
		struct penaudio_event *pe = &sc->sc_pm_q[PM_QSLOT(first + i)];

		if (pe->pe_tick <= sc->sc_pm_tick) {
			pmusic_pass_event(sc, &pp, pe);
			continue;
		}
		if (pe->pe_tick < last)
			break;
		last = pe->pe_tick;
		if (i != queued)
			sc->sc_pm_q[PM_QSLOT(first + queued)] = *pe;
		queued++;
	}
	if (i < staged)
		refused_tick = sc->sc_pm_q[PM_QSLOT(first + i)].pe_tick;
	refused = i < ps->ps_nevents;

	/* A refused first event is an error, and then nothing changes. */
	if (i == 0 && i < staged) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return EINVAL;
	}

	sc->sc_pm_qcount += queued;
	if (queued > 0)
		sc->sc_pm_qlast = last;
	pmusic_pass_end(sc, &pp);

	/* The horizon covers only what was taken: a tick with events
	 * still to come is not yet submitted. */
	through = ps->ps_through;
	if (refused)
		through = refused_tick > 0 ? MIN(through, refused_tick - 1) :
		    sc->sc_pm_through;
	sc->sc_pm_through = MAX(sc->sc_pm_through, through);
	pmusic_unstall(sc);

	pmusic_topup(sc);
	pmusic_status(sc, &ps->ps_status);
	mutex_spin_exit(&sc->sc_intr_lock);

	ps->ps_nevents = i;
	return 0;
}

static int
pmusic_start(struct paudio_softc *sc, const struct penaudio_clock *pc)
{
	u_int now;

	if (pc->pc_period_us < PENAUDIO_PERIOD_MIN_US)
		return EINVAL;

	mutex_spin_enter(&sc->sc_intr_lock);
	if (sc->sc_pm_started) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return EBUSY;
	}
	now = getticks();
	pmusic_set_period(sc, pc->pc_period_us);
	sc->sc_pm_lowat = pc->pc_lowat;
	sc->sc_pm_started = true;
	sc->sc_pm_due_hz = now;
	sc->sc_pm_due_us = 0;
	pmusic_due_advance(sc);
	if (sc->sc_pm_paused)
		sc->sc_pm_paused_at = now;
	else
		callout_schedule(&sc->sc_pm_clock, pmusic_due(sc) - now);
	mutex_spin_exit(&sc->sc_intr_lock);
	return 0;
}

/*
 * Freeze or thaw the clock and the session's channels together.  A
 * frozen channel holds its queue, so the output resumes on the frame
 * it stopped at, and the clock's timeline shifts by the time frozen.
 */
static void
pmusic_pause(struct paudio_softc *sc, bool pause)
{
	uint32_t sounding = 0;
	u_int now, shift, v;

	mutex_spin_enter(&sc->sc_intr_lock);
	if (pause == sc->sc_pm_paused) {
		mutex_spin_exit(&sc->sc_intr_lock);
		return;
	}
	now = getticks();

	if (pause) {
		sc->sc_pm_paused = true;
		sc->sc_pm_paused_at = now;
		paudio_chan_update(sc, PA_CHAN_EN, sc->sc_pm_chans, false);
		callout_stop(&sc->sc_pm_clock);
		mutex_spin_exit(&sc->sc_intr_lock);
		return;
	}

	sc->sc_pm_paused = false;
	for (v = 0; v < sc->sc_pm_nvoices; v++)
		if (sc->sc_pm_voice[v].pv_sounding)
			sounding |= PA_CHBIT(sc->sc_pm_voice[v].pv_chan);
	paudio_chan_update(sc, PA_CHAN_EN, sounding, true);

	shift = now - sc->sc_pm_paused_at;
	if (sc->sc_pm_started && sc->sc_pm_stalled) {
		sc->sc_pm_stall_at += shift;
	} else if (sc->sc_pm_started) {
		sc->sc_pm_due_hz += shift;
		callout_schedule(&sc->sc_pm_clock,
		    MAX(1, (int)(pmusic_due(sc) - now)));
	}
	mutex_spin_exit(&sc->sc_intr_lock);
}

static void
pmusic_reset(struct paudio_softc *sc)
{
	struct pmusic_pass pp = { 0, 0, 0 };

	mutex_spin_enter(&sc->sc_intr_lock);
	callout_stop(&sc->sc_pm_clock);
	sc->sc_pm_qcount = 0;
	sc->sc_pm_started = false;
	sc->sc_pm_paused = false;
	sc->sc_pm_stalled = false;
	sc->sc_pm_tick = 0;
	sc->sc_pm_through = 0;
	pp.pp_stop = sc->sc_pm_chans;
	pmusic_pass_end(sc, &pp);
	mutex_spin_exit(&sc->sc_intr_lock);

	pmusic_notify(sc);
}

/* ── Device entry points ──────────────────────────────────────── */

static int
pmusic_open(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct paudio_softc *sc = pmusic_lookup(dev);
	struct penaudio_event *q;

	if (sc == NULL)
		return ENXIO;

	q = kmem_alloc(PM_MAXEVENTS * sizeof(*q), KM_SLEEP);

	rw_enter(&sc->sc_pm_lock, RW_WRITER);
	if (sc->sc_pm_open) {
		rw_exit(&sc->sc_pm_lock);
		kmem_free(q, PM_MAXEVENTS * sizeof(*q));
		return EBUSY;
	}
	sc->sc_pm_open = true;

	mutex_spin_enter(&sc->sc_intr_lock);
	sc->sc_pm_q = q;
	sc->sc_pm_qhead = 0;
	sc->sc_pm_qcount = 0;
	sc->sc_pm_started = false;
	sc->sc_pm_paused = false;
	sc->sc_pm_stalled = false;
	sc->sc_pm_tick = 0;
	sc->sc_pm_through = 0;
	sc->sc_pm_lowat = 0;
	sc->sc_pm_stalls = 0;
	sc->sc_pm_underruns = 0;
	mutex_spin_exit(&sc->sc_intr_lock);

	rw_exit(&sc->sc_pm_lock);
	return 0;
}

/* The last close of the one open, so the session ends here however
 * its holders went away. */
static int
pmusic_close(dev_t dev, int flag, int mode, struct lwp *l)
{
	struct paudio_softc *sc = pmusic_lookup(dev);

	rw_enter(&sc->sc_pm_lock, RW_WRITER);

	mutex_spin_enter(&sc->sc_intr_lock);
	sc->sc_pm_started = false;
	sc->sc_pm_qcount = 0;
	pmusic_release(sc);
	mutex_spin_exit(&sc->sc_intr_lock);

	/* Stopped, so a callout already running returns without looking
	 * at the queue; wait for it before the queue goes. */
	callout_halt(&sc->sc_pm_clock, NULL);

	if (sc->sc_pm_mem != NULL)
		kmem_free(sc->sc_pm_mem, sc->sc_pm_memsize);
	sc->sc_pm_mem = NULL;
	sc->sc_pm_memsize = 0;
	kmem_free(sc->sc_pm_q, PM_MAXEVENTS * sizeof(*sc->sc_pm_q));
	sc->sc_pm_q = NULL;
	sc->sc_pm_open = false;

	rw_exit(&sc->sc_pm_lock);
	return 0;
}

/*
 * Sample memory, at the transfer's offset.  The refill may be reading
 * the same bytes from the interrupt; a sample rewritten while it plays
 * is a request the interface makes, and a torn frame is audio.
 */
static int
pmusic_rw(dev_t dev, struct uio *uio)
{
	struct paudio_softc *sc = pmusic_lookup(dev);
	off_t off = uio->uio_offset;
	int error = 0;

	if (off < 0)
		return EINVAL;

	rw_enter(&sc->sc_pm_lock, RW_READER);
	if (off >= sc->sc_pm_memsize) {
		if (uio->uio_rw == UIO_WRITE && uio->uio_resid > 0)
			error = ENOSPC;
	} else {
		error = uiomove(sc->sc_pm_mem + off,
		    MIN(uio->uio_resid, sc->sc_pm_memsize - off), uio);
	}
	rw_exit(&sc->sc_pm_lock);
	return error;
}

static int
pmusic_read(dev_t dev, struct uio *uio, int flag)
{
	return pmusic_rw(dev, uio);
}

static int
pmusic_write(dev_t dev, struct uio *uio, int flag)
{
	return pmusic_rw(dev, uio);
}

static int
pmusic_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct paudio_softc *sc = pmusic_lookup(dev);
	int error = 0;

	rw_enter(&sc->sc_pm_lock, RW_WRITER);
	switch (cmd) {
	case PENAUDIO_GETINFO:
		pmusic_getinfo(sc, data);
		break;
	case PENAUDIO_ALLOC:
		error = pmusic_alloc(sc, data);
		break;
	case PENAUDIO_SETMEM:
		error = pmusic_setmem(sc, *(uint32_t *)data);
		break;
	case PENAUDIO_SUBMIT:
		error = pmusic_submit(sc, data);
		break;
	case PENAUDIO_GETSTATUS:
		mutex_spin_enter(&sc->sc_intr_lock);
		pmusic_status(sc, data);
		mutex_spin_exit(&sc->sc_intr_lock);
		break;
	case PENAUDIO_START:
		error = pmusic_start(sc, data);
		break;
	case PENAUDIO_PAUSE:
		pmusic_pause(sc, *(int *)data != 0);
		break;
	case PENAUDIO_RESET:
		pmusic_reset(sc);
		break;
	case FIONBIO:
		/* No call waits, so there is nothing to make non-blocking. */
		break;
	default:
		error = ENOTTY;
		break;
	}
	rw_exit(&sc->sc_pm_lock);
	return error;
}

static int
pmusic_poll(dev_t dev, int events, struct lwp *l)
{
	struct paudio_softc *sc = pmusic_lookup(dev);
	int revents = 0;
	bool writable;

	if ((events & (POLLOUT | POLLWRNORM)) == 0)
		return 0;

	mutex_enter(&sc->sc_pm_klock);
	mutex_spin_enter(&sc->sc_intr_lock);
	writable = pmusic_writable(sc);
	mutex_spin_exit(&sc->sc_intr_lock);
	if (writable)
		revents = events & (POLLOUT | POLLWRNORM);
	else
		selrecord(l, &sc->sc_pm_wsel);
	mutex_exit(&sc->sc_pm_klock);

	return revents;
}

static void
filt_pmusic_detach(struct knote *kn)
{
	struct paudio_softc *sc = kn->kn_hook;

	mutex_enter(&sc->sc_pm_klock);
	selremove_knote(&sc->sc_pm_wsel, kn);
	mutex_exit(&sc->sc_pm_klock);
}

static int
filt_pmusic_write(struct knote *kn, long hint)
{
	struct paudio_softc *sc = kn->kn_hook;
	bool writable;

	mutex_spin_enter(&sc->sc_intr_lock);
	kn->kn_data = PM_MAXEVENTS - sc->sc_pm_qcount;
	writable = pmusic_writable(sc);
	mutex_spin_exit(&sc->sc_intr_lock);

	return writable;
}

static const struct filterops pmusic_write_filtops = {
	.f_flags	= FILTEROP_ISFD | FILTEROP_MPSAFE,
	.f_attach	= NULL,
	.f_detach	= filt_pmusic_detach,
	.f_event	= filt_pmusic_write,
};

static int
pmusic_kqfilter(dev_t dev, struct knote *kn)
{
	struct paudio_softc *sc = pmusic_lookup(dev);

	if (kn->kn_filter != EVFILT_WRITE)
		return EINVAL;

	kn->kn_fop = &pmusic_write_filtops;
	kn->kn_hook = sc;

	mutex_enter(&sc->sc_pm_klock);
	selrecord_knote(&sc->sc_pm_wsel, kn);
	mutex_exit(&sc->sc_pm_klock);
	return 0;
}

/* ── Attachment ───────────────────────────────────────────────── */

static int
pmusic_sysctl_maxmem(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct paudio_softc *sc = node.sysctl_data;
	int t = sc->sc_pm_maxmem;
	int error;

	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (t < 0)
		return EINVAL;
	sc->sc_pm_maxmem = t;
	return 0;
}

void
pmusic_attach(struct paudio_softc *sc, const struct sysctlnode *node)
{
	uint32_t cap = PA_RD(sc, PA_CAP);
	u_int pages;

	rw_init(&sc->sc_pm_lock);
	mutex_init(&sc->sc_pm_klock, MUTEX_DEFAULT, IPL_NONE);
	selinit(&sc->sc_pm_wsel);
	callout_init(&sc->sc_pm_clock, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_pm_clock, pmusic_clock, sc);
	evcnt_attach_dynamic(&sc->sc_ev_pm_intr, EVCNT_TYPE_INTR, NULL,
	    device_xname(sc->sc_dev), "pmusic intr");

	/* A voice needs a pitch and a gain of its own; without them a
	 * channel is only a second copy of the stream. */
	if ((cap & PA_CAP_PITCH) && (cap & PA_CAP_VOLUME))
		sc->sc_pm_nvoices_max = sc->sc_nchan;

	pages = 1U << fls32(physmem - 1);
	sc->sc_pm_maxmem = ctob(pages / PM_MAXMEM_SHARE);

	sc->sc_pm_ready = true;

	if (node == NULL)
		return;
	sysctl_createv(NULL, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "maxmem",
	    SYSCTL_DESCR("largest /dev/pmusic sample memory, bytes"),
	    pmusic_sysctl_maxmem, 0, (void *)sc, 0,
	    CTL_CREATE, CTL_EOL);
}

/*	$NetBSD$	*/

/*
 * Penumbra audio device (CLASS_AUDIO) audio(4) driver.
 *
 * The device is a pushed-PCM channel: a FIFO the CPU writes frames
 * into, a free-space watermark that raises an interrupt, and an
 * underrun flag (doc/system/devices/audio.md).  There is no DMA
 * engine, so the driver is the transport — every frame reaching the
 * jack passes through this file.
 *
 * That shape maps onto trigger_output rather than start_output: the
 * framework hands over a ring and a block size and asks to be called
 * once per block consumed, which is what a device that reads
 * continuously wants.  The block size becomes the channel's watermark,
 * so the interrupt arrives exactly when a block's worth of space has
 * opened up, and the FIFO depth is what bounds the block size this
 * driver will agree to in round_blocksize.
 *
 * Only playback exists.  The class has no capture side.
 *
 * audio(4) plays through the mandated channel, channel 0.  The
 * device's channels are also voices for /dev/pmusic (pmusic.c), and
 * the two share the device: channel 0 goes to whichever claims it
 * first, and the global registers are updated a channel at a time.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/intr.h>
#include <sys/mutex.h>
#include <sys/evcnt.h>
#include <sys/sysctl.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

#include <dev/audio/audio_if.h>

#include <arch/penumbra/penumbra/paudiovar.h>

#define	PA_TONE_HZ		440

#define	PA_CRD(sc, r)	PA_CHRD(sc, PA_CHAN0, r)
#define	PA_CWR(sc, r, v) PA_CHWR(sc, PA_CHAN0, r, v)

static int	paudio_match(device_t, cfdata_t, void *);
static void	paudio_attach(device_t, device_t, void *);
static int	paudio_intr(void *);
static const struct sysctlnode *paudio_sysctl_attach(struct paudio_softc *);
static void	paudio_selftest(struct paudio_softc *);

CFATTACH_DECL_NEW(paudio, sizeof(struct paudio_softc),
    paudio_match, paudio_attach, NULL, NULL);

/* ── Shared registers ─────────────────────────────────────────── */

/*
 * Set or clear channels' bits in a per-channel global register
 * (CHAN_EN, IRQ_ENABLE), leaving every other channel's alone: the
 * stream and the voices own different bits of the same words.
 * Called with sc_intr_lock held.
 */
void
paudio_chan_update(struct paudio_softc *sc, bus_size_t reg, uint32_t mask,
    bool on)
{
	uint32_t v = PA_RD(sc, reg);

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	PA_WR(sc, reg, on ? (v | mask) : (v & ~mask));
}

/*
 * The output stage runs while anything holds a channel, rather than
 * while any channel is enabled: voices start and stop far more often
 * than a session opens, and every start would otherwise switch the
 * whole output on again.  Called with sc_intr_lock held.
 */
void
paudio_output_update(struct paudio_softc *sc)
{
	bool on = sc->sc_running || sc->sc_pm_chans != 0;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	PA_WR(sc, PA_CTRL, on ? PA_CTRL_ENABLE : 0);
}

/* ── audio(4) interface ───────────────────────────────────────── */

/*
 * Channel 0 is a stream's only channel and may also be a voice, so it
 * belongs to whichever claims it first; /dev/pmusic takes it last.
 */
static int
paudio_open(void *hdl, int flags)
{
	struct paudio_softc *sc = hdl;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	if (sc->sc_pm_chans & __BIT(PA_CHAN0))
		return EBUSY;
	sc->sc_audio_open = true;
	return 0;
}

static void
paudio_close(void *hdl)
{
	struct paudio_softc *sc = hdl;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	sc->sc_audio_open = false;
}

static int
paudio_query_format(void *hdl, audio_format_query_t *afp)
{
	struct paudio_softc *sc = hdl;
	static const struct audio_format fmt = {
		.mode		= AUMODE_PLAY,
		.encoding	= AUDIO_ENCODING_SLINEAR_LE,
		.validbits	= 16,
		.precision	= 16,
		.channels	= 2,
		.channel_mask	= AUFMT_STEREO,
		.frequency_type	= 1,
	};
	struct audio_format f = fmt;

	if (afp->index != 0)
		return EINVAL;

	/* The device presents one rate; a consumer needing another
	 * resamples, which the framework does on its own. */
	f.frequency[0] = sc->sc_rate;
	afp->fmt = f;
	return 0;
}

static int
paudio_set_format(void *hdl, int setmode,
    const audio_params_t *play, const audio_params_t *rec,
    audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	/* query_format offers exactly what the hardware does, so
	 * whatever the framework selected needs no conversion. */
	return 0;
}

static int
paudio_round_blocksize(void *hdl, int bs, int mode,
    const audio_params_t *param)
{
	struct paudio_softc *sc = hdl;
	int maxbytes = (sc->sc_depth / 2) * PA_FRAME_SIZE;

	/*
	 * Half the queue.  The block becomes the watermark, so what
	 * stays queued when the interrupt arrives is the deadline for
	 * servicing it, and half spends the queue evenly between the
	 * two.
	 *
	 * Enlarging it past that buys nothing against blocks the mixer
	 * loses to a writer holding the track: a larger block means
	 * proportionally fewer mixer passes but a proportionally longer
	 * gap when one is lost, and the product — the share of the
	 * output that is silence — is the writer's lock duty cycle
	 * either way.  Shorter gaps are the better half of that trade.
	 */
	if (bs > maxbytes)
		bs = maxbytes;

	bs = rounddown(bs, PA_FRAME_SIZE);
	if (bs < PA_FRAME_SIZE)
		bs = PA_FRAME_SIZE;

	return bs;
}

static int
paudio_get_props(void *hdl)
{
	return AUDIO_PROP_PLAYBACK;
}

static int
paudio_getdev(void *hdl, struct audio_device *adp)
{
	strlcpy(adp->name, "Penumbra", sizeof(adp->name));
	strlcpy(adp->version, "1", sizeof(adp->version));
	strlcpy(adp->config, "paudio", sizeof(adp->config));
	return 0;
}

/*
 * No mixer: the class gates volume behind a capability this revision
 * does not advertise, and reporting controls that cannot move would
 * only mislead a mixer client.
 */
static int
paudio_set_port(void *hdl, mixer_ctrl_t *mc)
{
	return EINVAL;
}

static int
paudio_get_port(void *hdl, mixer_ctrl_t *mc)
{
	return EINVAL;
}

static int
paudio_query_devinfo(void *hdl, mixer_devinfo_t *di)
{
	return ENXIO;
}

static void
paudio_get_locks(void *hdl, kmutex_t **intr, kmutex_t **thread)
{
	struct paudio_softc *sc = hdl;

	*intr = &sc->sc_intr_lock;
	*thread = &sc->sc_lock;
}

/*
 * A triangle, for the test-tone diagnostic: no table and no libm, and
 * its phase is 16.16 so only the low half selects a point in the cycle.
 */
static uint32_t
paudio_tone(uint32_t phase)
{
	uint32_t p = phase & 0xFFFF;
	int32_t v = (p < 0x8000) ? (int32_t)(p * 2) - 0x8000
				 : 0x17FFF - (int32_t)(p * 2);

	return (uint32_t)(uint16_t)(int16_t)(v >> 3);
}

/*
 * Move frames from the ring into the channel FIFO.
 *
 * `sc_cur` is the read cursor and wraps at `sc_end`; the ring is a
 * byte buffer of whole frames, and one frame is one FIFO word — 16-bit
 * stereo with left in the low half — so frames go into the port
 * without repacking.
 *
 * CHAN[0].LEVEL is the free space in frames and bounds how much may be
 * written; anything past it is discarded rather than queued.
 *
 * `maxframes` caps the transfer independently of the queue's free
 * space, for a caller that can only account for so much.
 *
 * Returns the number of bytes written, which the caller uses to decide
 * when a block has been consumed and the framework is owed a callback.
 * Every byte this returns has left the ring, so a caller that discards
 * the count desynchronises the framework from the read cursor.
 * Called with sc_intr_lock held.
 */
static int
paudio_fill(struct paudio_softc *sc, u_int maxframes)
{
	u_int space = MIN(PA_CRD(sc, PA_C_LEVEL), maxframes);
	int written = 0;			/* bytes */

	KASSERT(((uintptr_t)sc->sc_cur % PA_FRAME_SIZE) == 0);

	while (space > 0) {
		u_int to_wrap = (sc->sc_end - sc->sc_cur) / PA_FRAME_SIZE;
		u_int nframes = MIN(space, to_wrap);

		/* One call per contiguous run: the source has to be
		 * contiguous, so a fill crossing the ring's end is two. */
		if (__predict_false(sc->sc_testtone)) {
			u_int inc = (PA_TONE_HZ * 65536u) / sc->sc_rate;
			u_int i;

			for (i = 0; i < nframes; i++) {
				uint32_t v = paudio_tone(sc->sc_tonephase);

				PA_CWR(sc, PA_C_FIFO, v | (v << 16));
				sc->sc_tonephase += inc;
			}
		} else {
			const uint32_t *p = (const uint32_t *)sc->sc_cur;
			u_int i;

			/*
			 * A run of frames that is entirely silence, in the
			 * middle of a stream that is not, is the framework
			 * substituting for a track it could not read —
			 * which sounds like a dropout the device never
			 * reports, because its queue never ran dry. The
			 * scan leaves on the first non-zero frame, so it
			 * costs nothing on material that is playing.
			 */
			for (i = 0; i < nframes && p[i] == 0; i++)
				continue;
			if (i == nframes)
				sc->sc_ev_silent.ev_count++;

			bus_space_write_multi_4(sc->sc_iot, sc->sc_ioh,
			    PA_CHAN(PA_CHAN0) + PA_C_FIFO, p, nframes);
		}

		sc->sc_cur += nframes * PA_FRAME_SIZE;
		if (sc->sc_cur == sc->sc_end)
			sc->sc_cur = sc->sc_start;

		written += nframes * PA_FRAME_SIZE;
		space -= nframes;
	}
	return written;
}

static int
paudio_trigger_output(void *hdl, void *start, void *end, int blksize,
    void (*intr)(void *), void *intrarg, const audio_params_t *param)
{
	struct paudio_softc *sc = hdl;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	sc->sc_start = start;
	sc->sc_end = end;
	sc->sc_cur = start;
	sc->sc_blksize = blksize;
	sc->sc_intr = intr;
	sc->sc_intrarg = intrarg;
	sc->sc_running = true;

	/* Start from a known-empty queue: a leftover frame from a
	 * previous stream would play before the first of this one. */
	PA_CWR(sc, PA_C_CTRL, PA_C_CTRL_FLUSH);
	PA_WR(sc, PA_UNDERRUN, __BIT(PA_CHAN0));

	/*
	 * Fill before enabling: a channel enabled dry underruns on its
	 * first output frame.  One block, because that is what can be
	 * accounted for without calling back into the framework from
	 * inside its own start path — the primed frames are the first
	 * block, just not yet finished.
	 */
	sc->sc_resid = blksize - paudio_fill(sc, blksize / PA_FRAME_SIZE);

	/* The watermark is one block of free space, so the interrupt
	 * arrives exactly when a whole block will fit. */
	PA_CWR(sc, PA_C_WM, blksize / PA_FRAME_SIZE);
	PA_WR(sc, PA_IRQ_STATUS, __BIT(PA_CHAN0));
	paudio_chan_update(sc, PA_IRQ_ENABLE, __BIT(PA_CHAN0), true);

	paudio_chan_update(sc, PA_CHAN_EN, __BIT(PA_CHAN0), true);
	paudio_output_update(sc);
	return 0;
}

static int
paudio_halt_output(void *hdl)
{
	struct paudio_softc *sc = hdl;

	KASSERT(mutex_owned(&sc->sc_intr_lock));

	paudio_chan_update(sc, PA_IRQ_ENABLE, __BIT(PA_CHAN0), false);
	paudio_chan_update(sc, PA_CHAN_EN, __BIT(PA_CHAN0), false);
	PA_CWR(sc, PA_C_CTRL, PA_C_CTRL_FLUSH);
	PA_CWR(sc, PA_C_WM, 0);
	PA_WR(sc, PA_IRQ_STATUS, __BIT(PA_CHAN0));

	sc->sc_running = false;
	sc->sc_intr = NULL;
	paudio_output_update(sc);
	return 0;
}

static const struct audio_hw_if paudio_hw_if = {
	.open		= paudio_open,
	.close		= paudio_close,
	.query_format	= paudio_query_format,
	.set_format	= paudio_set_format,
	.round_blocksize = paudio_round_blocksize,
	.trigger_output	= paudio_trigger_output,
	.halt_output	= paudio_halt_output,
	.getdev		= paudio_getdev,
	.set_port	= paudio_set_port,
	.get_port	= paudio_get_port,
	.query_devinfo	= paudio_query_devinfo,
	.get_props	= paudio_get_props,
	.get_locks	= paudio_get_locks,
};

/* ── Interrupt ────────────────────────────────────────────────── */

static void
paudio_intr_stream(struct paudio_softc *sc)
{
	int wrote;

	sc->sc_ev_intr.ev_count++;

	/* Sticky since the last look, so this counts servicings that
	 * arrived too late rather than output frames lost. */
	if (PA_RD(sc, PA_UNDERRUN) & __BIT(PA_CHAN0)) {
		sc->sc_ev_underrun.ev_count++;
		PA_WR(sc, PA_UNDERRUN, __BIT(PA_CHAN0));
	}

	/*
	 * One block, never more.  The framework produces exactly one
	 * block per callback and keeps no deeper lookahead, so frames
	 * past that are ring space it has not written yet — reading
	 * them plays stale audio without the device ever running dry.
	 */
	wrote = paudio_fill(sc, sc->sc_blksize / PA_FRAME_SIZE);

	/*
	 * The framework is owed one callback per block consumed. Written
	 * bytes can span a block boundary, so this counts down rather
	 * than comparing against the block size.
	 */
	while (wrote >= sc->sc_resid) {
		wrote -= sc->sc_resid;
		sc->sc_resid = sc->sc_blksize;
		sc->sc_ev_block.ev_count++;
		if (sc->sc_intr != NULL)
			sc->sc_intr(sc->sc_intrarg);
	}
	sc->sc_resid -= wrote;

	/*
	 * Clear last: the flag is level-sensitive on free space, so a
	 * clear before the refill would immediately set again and the
	 * device would interrupt without new work.
	 */
	PA_WR(sc, PA_IRQ_STATUS, __BIT(PA_CHAN0));
}

static int
paudio_intr(void *arg)
{
	struct paudio_softc *sc = arg;
	uint32_t status;
	int handled = 0;

	mutex_enter(&sc->sc_intr_lock);

	status = PA_RD(sc, PA_IRQ_STATUS);
	if ((status & __BIT(PA_CHAN0)) != 0 && sc->sc_running) {
		paudio_intr_stream(sc);
		handled = 1;
	}
	handled |= pmusic_intr(sc, status);

	mutex_exit(&sc->sc_intr_lock);
	return handled;
}

/* ── Diagnostics ──────────────────────────────────────────────── */

/*
 * Verify a burst write moves the queue by exactly one frame per word.
 *
 * write_multi_4 stores to a single address in a tight loop, which is
 * an access pattern nothing else on this bus uses; a store merged or
 * repeated there would corrupt audio in a way no listening test
 * localises, because a smooth waveform hides a lost frame. The queue
 * does not drain while the output stage is disabled, so the count is
 * exact rather than a race against the converter.
 */
static void
paudio_selftest(struct paudio_softc *sc)
{
	uint32_t pattern[16];
	uint32_t before, after;
	u_int i, n = __arraycount(pattern);

	for (i = 0; i < n; i++)
		pattern[i] = 0x11110000U + i;

	PA_CWR(sc, PA_C_CTRL, PA_C_CTRL_FLUSH);
	before = PA_CRD(sc, PA_C_LEVEL);

	bus_space_write_multi_4(sc->sc_iot, sc->sc_ioh,
	    PA_CHAN(PA_CHAN0) + PA_C_FIFO, pattern, n);

	after = PA_CRD(sc, PA_C_LEVEL);
	PA_CWR(sc, PA_C_CTRL, PA_C_CTRL_FLUSH);

	if (before - after != n)
		aprint_error_dev(sc->sc_dev,
		    "burst of %u frames moved the queue by %u\n",
		    n, before - after);
}

static const struct sysctlnode *
paudio_sysctl_attach(struct paudio_softc *sc)
{
	const struct sysctlnode *node = NULL;

	sysctl_createv(NULL, 0, NULL, &node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "paudio",
	    SYSCTL_DESCR("Penumbra audio device"),
	    NULL, 0, NULL, 0,
	    CTL_MACHDEP, CTL_CREATE, CTL_EOL);

	if (node == NULL)
		return NULL;

	sysctl_createv(NULL, 0, &node, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "testtone",
	    SYSCTL_DESCR("play a generated tone instead of the stream, "
		"to tell a fault above this driver from one below it"),
	    NULL, 0, &sc->sc_testtone, 0,
	    CTL_CREATE, CTL_EOL);

	return node;
}

/* ── Autoconfiguration ────────────────────────────────────────── */

static int
paudio_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_AUDIO;
}

static void
paudio_attach(device_t parent, device_t self, void *aux)
{
	struct paudio_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	uint32_t cap;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map audio registers: %d\n", error);
		return;
	}

	cap = PA_RD(sc, PA_CAP);
	sc->sc_rate = PA_RD(sc, PA_RATE);
	sc->sc_depth = PA_CRD(sc, PA_C_DEPTH);

	aprint_naive("\n");
	aprint_normal(": Penumbra audio (v%u, %u ch, %u Hz, %u-frame queue)\n",
	    PA_CAP_VERSION(cap), PA_CAP_CHANNELS(cap), sc->sc_rate,
	    sc->sc_depth);

	if (PA_CAP_VERSION(cap) != 1) {
		aprint_error_dev(self, "unsupported version\n");
		return;
	}
	if (sc->sc_rate == 0 || sc->sc_depth == 0) {
		aprint_error_dev(self, "device reports no rate or no queue\n");
		return;
	}
	sc->sc_nchan = MIN(PA_CAP_CHANNELS(cap), PA_MAXCHAN);

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	/* Silent until a stream starts, whatever state a reset left. */
	PA_WR(sc, PA_CTRL, 0);
	PA_WR(sc, PA_CHAN_EN, 0);
	PA_WR(sc, PA_IRQ_ENABLE, 0);
	PA_WR(sc, PA_IRQ_STATUS, ~0U);
	PA_WR(sc, PA_UNDERRUN, ~0U);
	PA_CWR(sc, PA_C_CTRL, PA_C_CTRL_FLUSH);
	PA_CWR(sc, PA_C_WM, 0);

	evcnt_attach_dynamic(&sc->sc_ev_intr, EVCNT_TYPE_INTR, NULL,
	    device_xname(self), "intr");
	evcnt_attach_dynamic(&sc->sc_ev_underrun, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "underrun");
	evcnt_attach_dynamic(&sc->sc_ev_block, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "blocks");
	evcnt_attach_dynamic(&sc->sc_ev_silent, EVCNT_TYPE_MISC, NULL,
	    device_xname(self), "silent blocks");

	paudio_selftest(sc);
	pmusic_attach(sc, paudio_sysctl_attach(sc));

	sc->sc_ih = intr_establish_xname(0, IPL_SCHED, paudio_intr, sc,
	    device_xname(self));
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "can't establish interrupt\n");
		return;
	}

	audio_attach_mi(&paudio_hw_if, sc, self);
}

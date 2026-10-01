/*	$NetBSD$	*/

/*
 * Penumbra audio device (CLASS_AUDIO): state shared by the audio(4)
 * binding in paudio.c, which drives channel 0 as a PCM stream, and the
 * voice device in pmusic.c, which drives channels as voices.
 */

#ifndef _PENUMBRA_PAUDIOVAR_H_
#define _PENUMBRA_PAUDIOVAR_H_

#include <sys/callout.h>
#include <sys/evcnt.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/select.h>
#include <sys/sysctl.h>

#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/penaudio.h>

/* Register offsets (doc/system/devices/audio.md). */
#define	PA_CAP			0x000
#define	PA_RATE			0x004
#define	PA_CTRL			0x008
#define	PA_CHAN_EN		0x00C
#define	PA_IRQ_STATUS		0x010
#define	PA_IRQ_ENABLE		0x014
#define	PA_UNDERRUN		0x018
#define	PA_STATUS		0x020
#define	PA_CHAN(n)		(0x100 + (n) * 0x20)
#define	PA_C_CTRL		0x00
#define	PA_C_INC		0x04
#define	PA_C_VOL		0x08
#define	PA_C_DEPTH		0x0C
#define	PA_C_LEVEL		0x10
#define	PA_C_WM			0x14
#define	PA_C_FIFO		0x18

#define	PA_CAP_VERSION(c)	((c) & 0xFF)
#define	PA_CAP_PITCH		__BIT(8)
#define	PA_CAP_VOLUME		__BIT(9)
#define	PA_CAP_STATUS		__BIT(13)
#define	PA_CAP_CHANNELS(c)	(((c) >> 16) & 0xFF)
#define	PA_CTRL_ENABLE		__BIT(0)
#define	PA_STATUS_BUSY		__BIT(0)
#define	PA_STATUS_FAULT		__BIT(1)
#define	PA_STATUS_FAULT_CODE(s)	(((s) >> 8) & 0xFF)
#define	PA_C_CTRL_FLUSH		__BIT(8)

/* How long the class lets STATUS.BUSY stay set after reset. */
#define	PA_STATUS_WAIT_MS	100

#define	PA_INC_UNITY		0x00010000
#define	PA_VOL_UNITY		0xFFFFFFFF

/* One bit per channel in a single word.  PA_CHBIT stays 32-bit for a
 * channel only known at run time, where __BIT would shift 64 bits. */
#define	PA_MAXCHAN		32
#define	PA_CHBIT(ch)		(1U << (ch))

/* The mandated channel, which audio(4) plays through. */
#define	PA_CHAN0		0

/* 16-bit signed stereo: one frame per FIFO word, left in the low half. */
#define	PA_FRAME_SIZE		4

/*
 * A region of sample memory as the driver uses it: a run of frames in
 * one format.  Events name regions in bytes; they become spans once,
 * when applied, so every position the driver keeps is a frame count.
 */
struct pmusic_span {
	uint32_t		sn_base;	/* byte offset of frame 0 */
	u_int			sn_frames;	/* 0 = nothing */
	uint32_t		sn_format;	/* PENAUDIO_FMT_* */
};

/*
 * One voice of the /dev/pmusic session: a channel, and where in sample
 * memory its refill has reached.
 */
struct pmusic_voice {
	u_int			pv_chan;	/* hardware channel */
	u_int			pv_depth;	/* its FIFO, in frames */

	/* The refill's pass, and the frames of it queued so far. */
	struct pmusic_span	pv_pass;
	u_int			pv_at;

	/* What follows the pass, repeating until changed. */
	struct pmusic_span	pv_loop;

	/*
	 * What the queue holds behind the refill's pass, for finding
	 * where the device is.  Since the channel last started from
	 * empty, it has been fed one first pass and then copies of the
	 * refill's pass: pv_head is that first pass, 0 frames while the
	 * refill is still in it, and pv_looped counts the frames of
	 * copies since, saturating at pv_depth, beyond which the first
	 * pass is out of the queue's reach.
	 */
	struct pmusic_span	pv_head;
	u_int			pv_looped;

	bool			pv_sounding;	/* has data to play */
	bool			pv_draining;	/* all of it queued */
};

struct paudio_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	void			*sc_ih;

	kmutex_t		sc_lock;	/* audio(4) thread lock */
	kmutex_t		sc_intr_lock;	/* device registers; interrupt */

	u_int			sc_rate;	/* device output rate */
	u_int			sc_play_rate;	/* rate the stream plays at */
	bool			sc_pitch;	/* INC is writable (CAP.PITCH) */
	u_int			sc_nchan;	/* channels the device has */
	u_int			sc_depth;	/* channel 0 FIFO, in frames */

	/* audio(4) holds channel 0 between its hw open and close. */
	bool			sc_audio_open;

	/* Ring handed over by trigger_output, and where we are in it. */
	uint8_t			*sc_start;
	uint8_t			*sc_end;
	uint8_t			*sc_cur;
	int			sc_blksize;	/* bytes per block */
	int			sc_resid;	/* bytes left in this block */
	void			(*sc_intr)(void *);
	void			*sc_intrarg;
	bool			sc_running;

	/*
	 * A skip in the output is either the device running dry or the
	 * driver mishandling the ring, and those want opposite fixes.
	 * UNDERRUN is the device's own answer to which one it was.
	 */
	struct evcnt		sc_ev_intr;
	struct evcnt		sc_ev_underrun;
	struct evcnt		sc_ev_block;
	struct evcnt		sc_ev_silent;

	/*
	 * Diagnostic: replace the ring's contents with a tone the driver
	 * generates, keeping the block accounting identical.  What comes
	 * out then depends only on the interrupt path, the FIFO writes
	 * and the converter — so a clean tone puts a fault above this
	 * driver and a dirty one puts it below.
	 */
	int			sc_testtone;
	uint32_t		sc_tonephase;

	/*
	 * /dev/pmusic.  sc_pm_lock serializes the session's calls, and
	 * is a reader/writer lock because they copy to and from user
	 * space while holding it.  What the clock and the interrupt
	 * touch is under sc_intr_lock.
	 */
	krwlock_t		sc_pm_lock;
	kmutex_t		sc_pm_klock;	/* sc_pm_wsel's knotes */
	struct selinfo		sc_pm_wsel;
	callout_t		sc_pm_clock;
	struct evcnt		sc_ev_pm_intr;

	bool			sc_pm_ready;	/* attached */
	u_int			sc_pm_nvoices_max;	/* 0: device can't */
	int			sc_pm_maxmem;		/* sysctl knob */
	bool			sc_pm_open;

	uint8_t			*sc_pm_mem;
	uint32_t		sc_pm_memsize;

	struct pmusic_voice	sc_pm_voice[PA_MAXCHAN];
	u_int			sc_pm_nvoices;	/* allocated */
	uint32_t		sc_pm_chans;	/* their channels */

	/* Event queue: a ring of PM_MAXEVENTS, ticks non-decreasing. */
	struct penaudio_event	*sc_pm_q;
	u_int			sc_pm_qhead;
	u_int			sc_pm_qcount;
	uint32_t		sc_pm_qlast;	/* tick of the newest */

	/*
	 * Tick clock.  Times are clock ticks as getticks() counts them,
	 * compared by difference so they wrap cleanly; a due time keeps
	 * the microseconds past its clock tick so periods that are not
	 * whole clock ticks accumulate without drift.
	 */
	bool			sc_pm_started;
	bool			sc_pm_paused;
	bool			sc_pm_stalled;
	uint32_t		sc_pm_tick;	/* now in effect */
	uint32_t		sc_pm_through;	/* horizon */
	uint32_t		sc_pm_lowat;
	u_int			sc_pm_period_hz;	/* period, whole clock ticks */
	uint32_t		sc_pm_period_us;	/*  ... and the rest */
	u_int			sc_pm_due_hz;	/* next tick is due at this */
	uint32_t		sc_pm_due_us;	/*  ... plus this */
	u_int			sc_pm_stall_at;	/* clock tick a stall began */
	u_int			sc_pm_paused_at;
	uint32_t		sc_pm_stalls;
	uint32_t		sc_pm_underruns;
};

#define	PA_RD(sc, r)	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define	PA_WR(sc, r, v)	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))
#define	PA_CHRD(sc, ch, r)	PA_RD(sc, PA_CHAN(ch) + (r))
#define	PA_CHWR(sc, ch, r, v)	PA_WR(sc, PA_CHAN(ch) + (r), (v))

void	paudio_chan_update(struct paudio_softc *, bus_size_t, uint32_t, bool);
void	paudio_output_update(struct paudio_softc *);

void	pmusic_attach(struct paudio_softc *, const struct sysctlnode *);
int	pmusic_intr(struct paudio_softc *, uint32_t);

#endif /* _PENUMBRA_PAUDIOVAR_H_ */

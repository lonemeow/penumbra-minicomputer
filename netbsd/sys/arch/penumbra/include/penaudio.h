/*	$NetBSD$	*/

/*
 * /dev/pmusic: the Penumbra audio device's channels as voices, played
 * from a sample memory by timed events.  The interface and the reasons
 * for its shape are in doc/system/netbsd/design-notes.md, "Audio
 * Driver".
 */

#ifndef _PENUMBRA_PENAUDIO_H_
#define _PENUMBRA_PENAUDIO_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define PENAUDIO_VOICE_NONE	0xFFFF

/* pe_format */
#define PENAUDIO_FMT_S8		1	/* signed 8-bit mono */
#define PENAUDIO_FMT_S16	2	/* signed 16-bit LE mono */
#define PENAUDIO_FMT_S16_STEREO	3	/* signed 16-bit LE, left then right */

/* The shortest tick period, so one clock pass applies a bounded number
 * of ticks. */
#define PENAUDIO_PERIOD_MIN_US	1000

/* pe_inc: the range every CLASS_AUDIO device supports, 1/256x to 4x. */
#define PENAUDIO_INC_MIN	0x00000100
#define PENAUDIO_INC_MAX	0x00040000

struct penaudio_info {
	uint32_t pi_rate;		/* output frames per second */
	uint16_t pi_nvoices;		/* voices the device has */
	uint16_t pi_navail;		/* voices PENAUDIO_ALLOC can grant now */
	uint32_t pi_maxmem;		/* largest sample memory, bytes */
	uint32_t pi_maxevents;		/* event queue capacity */
	uint32_t pi_tick_grain_us;	/* ticks land on multiples of this */
};

struct penaudio_region {
	uint32_t pr_offset;		/* bytes into sample memory */
	uint32_t pr_length;		/* bytes; 0 = nothing */
};

/* pe_set: which fields apply, in this order within one event */
#define PENAUDIO_SET_PERIOD	0x0001	/* tick period; voice NONE */
#define PENAUDIO_SET_INC	0x0002
#define PENAUDIO_SET_VOL	0x0004
#define PENAUDIO_SET_PLAY	0x0008	/* discard queued, play pe_play, then pe_loop */
#define PENAUDIO_SET_LOOP	0x0010	/* pe_loop follows the current pass */
#define PENAUDIO_SET_STOP	0x0020	/* discard queued, fall silent */

struct penaudio_event {
	uint32_t pe_tick;		/* tick this applies on */
	uint16_t pe_voice;		/* voice within the allocation */
	uint16_t pe_set;		/* PENAUDIO_SET_* */
	uint32_t pe_inc;		/* 16.16 source frames per output frame */
	uint16_t pe_vol_l;		/* 0x0000 silent .. 0xFFFF unity */
	uint16_t pe_vol_r;
	uint32_t pe_format;		/* for pe_play and pe_loop */
	struct penaudio_region pe_play;
	struct penaudio_region pe_loop;	/* repeats until changed; 0 = one-shot */
	uint32_t pe_period_us;		/* SET_PERIOD */
};

struct penaudio_status {
	uint32_t pst_tick;		/* tick now in effect */
	uint32_t pst_through;		/* horizon as last declared */
	uint32_t pst_room;		/* events the queue can take now */
	uint32_t pst_stalls;		/* clock interrupts spent at the horizon */
	uint32_t pst_underruns;		/* channels found empty mid-sound */
	uint32_t pst_sounding;		/* bit n: voice n has data to play */
};

struct penaudio_submit {
	const struct penaudio_event *ps_events;
	uint32_t ps_eventsize;		/* sizeof(struct penaudio_event) */
	uint32_t ps_nevents;		/* in: offered; out: accepted */
	uint32_t ps_through;		/* in: every event up to this tick is submitted */
	struct penaudio_status ps_status;	/* out */
};

struct penaudio_clock {
	uint32_t pc_period_us;		/* until a SET_PERIOD event */
	uint32_t pc_lowat;		/* POLLOUT once fewer ticks than this are queued */
};

#define PENAUDIO_GETINFO	_IOR('Y', 0, struct penaudio_info)
#define PENAUDIO_ALLOC		_IOWR('Y', 1, uint32_t)	/* voices wanted/granted; 0 releases */
#define PENAUDIO_SETMEM		_IOW('Y', 2, uint32_t)	/* sample memory size, bytes */
#define PENAUDIO_SUBMIT		_IOWR('Y', 3, struct penaudio_submit)
#define PENAUDIO_GETSTATUS	_IOR('Y', 4, struct penaudio_status)
#define PENAUDIO_START		_IOW('Y', 5, struct penaudio_clock)
#define PENAUDIO_PAUSE		_IOW('Y', 6, int)	/* 1 freezes clock and voices, 0 resumes */
#define PENAUDIO_RESET		_IO('Y', 7)		/* drop queue, silence voices, clock to 0 */

#endif /* _PENUMBRA_PENAUDIO_H_ */

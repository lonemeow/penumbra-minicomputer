/*
 * A software mixer: the voices a song's ticks describe, mixed into
 * 16-bit stereo, for when a song has more channels than the audio
 * device has voices.
 *
 * It plays sample memory the way the device's voices do (penaudio.h):
 * a start plays its first pass and then repeats the loop that follows
 * it, the position running on past a pass's end into what comes next,
 * and a loop latched during a pass takes over when that pass ends.
 */

#ifndef MODPLAY_MIXER_H
#define MODPLAY_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#include "fixed.h"
#include "replay.h"

#define	MIXER_BLOCK	128	/* frames mixed at a time */
#define	MIXER_FRAME_BYTES 4	/* 16-bit stereo */

struct mixer_voice {
	bool		sounding;
	fix32_t		pos;		/* frames into sample memory */
	fix32_t		inc;		/* frames per output frame */
	int32_t		end;		/* frame the pass in progress ends at */
	struct replay_region loop;	/* repeats after it; empty = none */
	int		period;		/* the one inc was worked out for */
	int32_t		gain;		/* each sample's multiplier */
	int		pan;
};

struct mixer {
	const int8_t	*mem;		/* sample memory */
	uint32_t	memsize;
	uint32_t	rate;
	fix32_t		clock_inc;	/* the increment at a period of 1 */
	int		voices;
	struct mixer_voice voice[REPLAY_VOICES];

	/* Each side's share of a bus, by pan, in 256ths. */
	int32_t		pan_left[2 * REPLAY_PAN + 1];
	int32_t		pan_right[2 * REPLAY_PAN + 1];

	uint32_t	pending;	/* frames of the tick left to mix */
	fix32_t		carry;		/* the part-frame it leaves over */

	/* Voices at the same pan sum into a bus, so a side's gain is
	 * applied once per bus rather than once per voice. */
	int32_t		bus[REPLAY_VOICES][MIXER_BLOCK];
};

/* Crossfeed is in percent, as modplay's -c takes it. */
void		 mixer_init(struct mixer *, const struct song *, uint32_t rate,
		     int crossfeed);

/* Take on what a tick's voices play; its frames are then pending. */
void		 mixer_tick(struct mixer *, const struct replay_tick *);

/* Mix up to max of the pending frames into out, interleaved left and
 * right; returns how many, 0 once the tick is mixed. */
uint32_t	 mixer_mix(struct mixer *, int16_t *out, uint32_t max);

/*
 * The same into a ring of capacity bytes, starting at byte at and
 * wrapping at its end, for a device whose own buffer the mix goes
 * straight into; returns the bytes mixed.  Byte counts rather than
 * frames, since that is what such a device counts in.
 */
uint32_t	 mixer_mix_ring(struct mixer *, uint8_t *ring, uint32_t capacity,
		     uint32_t at, uint32_t max);

#endif /* MODPLAY_MIXER_H */

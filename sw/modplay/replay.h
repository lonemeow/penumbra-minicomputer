/*
 * A ProTracker replayer, as a sequence of ticks.
 *
 * Each call yields one tick: where it falls in the song, its tempo, and
 * what each channel plays during it, in the module's own terms — Amiga
 * periods, volumes of 0..64, and sample starts.  Turning that into
 * sound is the caller's business.
 */

#ifndef MODPLAY_REPLAY_H
#define MODPLAY_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "mod.h"

struct replay_voice {
	bool		trigger;	/* the sample restarts this tick */
	uint32_t	offset;		/*  ... this many bytes in */
	bool		swap;		/* a new sample follows the current pass */
	int		sample;		/* 1..31; 0 = none yet */
	int		period;		/* Amiga period; 0 = nothing to play */
	int		volume;		/* 0..64 */
};

struct replay_tick {
	int		order, pattern, row, tick;
	int		speed;		/* ticks per row */
	int		bpm;		/* tempo: a tick lasts 2.5 s / bpm */
	struct replay_voice voice[MOD_CHANNELS];
};

struct replay_chan {
	int		sample;
	int		finetune;
	int		period;		/* before vibrato and arpeggio */
	int		volume;		/* before tremolo */
	int		out_period;	/* this tick's, after them */
	int		out_volume;

	uint8_t		effect, param;	/* this row's */
	int		delay;		/* tick a delayed note starts; 0 = none */
	int		porta_target, porta_speed;
	int		vib_pos, vib_speed, vib_depth;
	int		trem_pos, trem_speed, trem_depth;
	uint32_t	offset;		/* sample offset, remembered */
};

struct replay {
	const struct mod *mod;
	bool		loop;		/* play forever rather than stop */
	bool		ended;
	int		order, row, tick;
	int		speed, bpm;

	/* Where the row after this one comes from, when not the next. */
	int		jump_order;	/* -1: the next order */
	int		break_row;	/* -1: row 0 */

	/* Times the current row repeats, without reading its notes again. */
	int		row_delay;
	bool		repeating;

	/* Rows already played, to find where the song comes round. */
	uint8_t		played[MOD_ORDERS][MOD_ROWS / 8];

	struct replay_chan chan[MOD_CHANNELS];
};

void		 replay_init(struct replay *, const struct mod *, bool loop);
bool		 replay_next(struct replay *, struct replay_tick *);

/* The effects the module uses that the replayer ignores, as a list of
 * their names, or NULL. */
const char	*replay_unsupported(const struct mod *);

#endif /* MODPLAY_REPLAY_H */

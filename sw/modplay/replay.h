/*
 * A ProTracker replayer, as a sequence of ticks.
 *
 * Each call yields one tick: where it falls in the song, its tempo, and
 * what each channel plays during it — Amiga periods, volumes of 0..64,
 * and the regions of sample memory each pass plays.  Which samples a
 * module holds and where each one's loop begins are the replayer's
 * business, so a tick describes sound rather than a sample table, and
 * turning it into sound is the caller's.
 */

#ifndef MODPLAY_REPLAY_H
#define MODPLAY_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "mod.h"

/* A tick lasts 2.5 s divided by the tempo, as ProTracker's CIA timer
 * runs it. */
#define	REPLAY_TICK_US(bpm)	(2500000 / (bpm))

/* A run of sample memory, in bytes; a length of 0 is nothing. */
struct replay_region {
	uint32_t	offset;
	uint32_t	length;
};

struct replay_voice {
	bool		trigger;	/* play restarts from `play` this tick */
	bool		relatch;	/* `loop` takes over after this pass */
	struct replay_region play;	/* on a trigger: the first pass */
	struct replay_region loop;	/* repeats after it; empty = one-shot */
	int		instrument;	/* what a display names; 0 = none yet */
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
	int		loop_row;	/* row a pattern loop returns to */
	int		loop_count;	/* passes left; 0 = none running */
	uint32_t	offset;		/* sample offset, remembered */
	uint32_t	trigger_offset;	/* where this tick's start plays from */
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
	int		loop_to;	/* a row of this order; -1: none */

	/* Times the current row repeats, without reading its notes again. */
	int		row_delay;
	bool		repeating;

	/* Rows already played, to find where the song comes round. */
	uint8_t		played[MOD_ORDERS][MOD_ROWS / 8];

	/* The order the song came round to, once it has; -1 while it
	 * plays on, and where it stops without repeating. */
	int		loop_order;

	struct replay_chan chan[MOD_CHANNELS];
};

/* Start is the order to begin at, which the caller keeps within the
 * song's length. */
void		 replay_init(struct replay *, const struct mod *, int start,
		     bool loop);
bool		 replay_next(struct replay *, struct replay_tick *);

/*
 * A cue: a run of orders entered as a unit, ending where it comes
 * round to an order it has already played.  A module holding a game's
 * music keeps many — one per tune, and one per jingle the game plays
 * over it — so the order a listener wants is rarely the first.
 */
struct replay_cue {
	int		start;		/* the order it is entered at */
	int		loop;		/* where it comes round; -1 = it stops */
	int		orders;		/* orders it visits */
	uint64_t	us;		/* one pass, up to where it comes round */
};

/* The cues a module holds, in order; returns how many there are, at
 * most max. */
int		 replay_scan(const struct mod *, struct replay_cue *, int max);

/* The effects the module uses that the replayer ignores, as a list of
 * their names, or NULL. */
const char	*replay_unsupported(const struct mod *);

#endif /* MODPLAY_REPLAY_H */

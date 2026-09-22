/*
 * A replayer, as a sequence of ticks.
 *
 * Each call yields one tick: where it falls in the song, its tempo, and
 * what each channel plays during it — periods of the song's clock,
 * volumes of 0..64, a place in the stereo field, and the regions of
 * sample memory each pass plays.  Which samples a song holds and where
 * each one's loop begins are the replayer's business, so a tick
 * describes sound rather than a sample table, and turning it into
 * sound is the caller's.
 */

#ifndef MODPLAY_REPLAY_H
#define MODPLAY_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "song.h"

/* A tick lasts 2.5 s divided by the tempo, as ProTracker's CIA timer
 * runs it. */
#define	REPLAY_TICK_US(bpm)	(2500000 / (bpm))

#define	REPLAY_VOICES	32

/* A voice's pan runs from -REPLAY_PAN, the left side alone, to
 * REPLAY_PAN, the right. */
#define	REPLAY_PAN	64

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
	int		period;		/* of the song's clock; 0 = nothing */
	int		volume;		/* 0..64 */
	int		pan;
};

struct replay_tick {
	int		order, pattern, row, tick;
	int		speed;		/* ticks per row */
	int		bpm;		/* tempo: a tick lasts 2.5 s / bpm */
	struct replay_voice voice[REPLAY_VOICES];	/* the song's channels */
};

struct replay {
	const struct song *song;
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
	uint8_t		played[SONG_ORDERS][SONG_ROWS / 8];

	/* The order the song came round to, once it has; -1 while it
	 * plays on, and where it stops without repeating. */
	int		loop_order;

	/* The format's own. */
	union {
		struct pt_state	pt;
		struct st3_state st3;
	};
};

/* Start is the order to begin at, which the caller keeps within the
 * song's length. */
void		 replay_init(struct replay *, const struct song *, int start,
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

/* The cues a song holds, in order; returns how many there are, at
 * most max. */
int		 replay_scan(const struct song *, struct replay_cue *, int max);

#endif /* MODPLAY_REPLAY_H */

/*
 * ProTracker modules: the 31-sample, four-channel format under its
 * common signatures, and what its replayer keeps for each channel.
 */

#ifndef MODPLAY_MOD_H
#define MODPLAY_MOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	MOD_CHANNELS	4
#define	MOD_SAMPLES	31
#define	MOD_ROWS	64
#define	MOD_ORDERS	128

/* Most modules sound harsh panned hard, as the Amiga plays them; this
 * leaves each channel clearly on its side. */
#define	MOD_CROSSFEED	75	/* percent */

struct replay;
struct replay_tick;
struct song;

struct mod_sample {
	char		name[23];
	uint32_t	offset;		/* bytes into the sample data */
	uint32_t	length;		/* bytes */
	uint32_t	loop_start;	/* bytes */
	uint32_t	loop_length;	/* bytes; 0 = does not loop */
	int		finetune;	/* -8..7, eighths of a semitone */
	int		volume;		/* 0..64 */
};

struct mod_note {
	uint16_t	period;		/* 0 = no note */
	uint8_t		sample;		/* 1..31; 0 = none */
	uint8_t		effect;		/* 0x0..0xF */
	uint8_t		param;
};

struct mod {
	struct mod_sample sample[MOD_SAMPLES + 1];	/* by number; [0] unused */
	struct mod_note	*pattern;	/* [npatterns][MOD_ROWS][MOD_CHANNELS] */
};

static inline const struct mod_note *
mod_note(const struct mod *m, int pattern, int row, int ch)
{
	return &m->pattern[(pattern * MOD_ROWS + row) * MOD_CHANNELS + ch];
}

/* ── Replay (pt.c) ─────────────────────────────────────────── */

struct pt_chan {
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

struct pt_state {
	struct pt_chan	chan[MOD_CHANNELS];
};

const char	*pt_unsupported(const struct song *);
void		 pt_start(struct replay *);
void		 pt_play(struct replay *, struct replay_tick *, bool row);

#endif /* MODPLAY_MOD_H */

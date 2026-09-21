/*
 * ProTracker replay semantics, one tick at a time.
 *
 * A row is read on its first tick, where notes start and the
 * once-a-row effects apply; the effects that move — slides, vibrato,
 * tremolo, arpeggio — act on each later tick of the row.  Where
 * trackers disagree, ProTracker 2.3 is the reference.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay.h"

#define	NOTES		36	/* C-1 to B-3 */
#define	PERIOD_MIN	113	/* B-3: where a portamento up stops */
#define	PERIOD_MAX	856	/* C-1: where one down stops */

/*
 * The notes at finetune 0, as the format defines them: patterns store
 * these numbers, so they are data rather than something to compute.
 */
static const int note_period[NOTES] = {
	856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
	428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
	214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

/* The notes by finetune, as its low nibble. */
static int period_table[16][NOTES];

/* Half a sine, 0..255: the vibrato and tremolo waveform. */
static int sine_table[32];

static void
tables_init(void)
{
	static bool done;
	int ft, note, i;

	if (done)
		return;
	/*
	 * A finetune step is an eighth of a semitone.  ProTracker's own
	 * finetuned tables were rounded their own way and differ from
	 * these by at most one period; what matters more is that a
	 * channel's period always comes from the table arpeggio searches.
	 */
	for (ft = -8; ft < 8; ft++) {
		/* 2^(-ft/96) in 16.16, the one libm call per finetune. */
		uint32_t scale = (uint32_t)lrint(65536.0 * pow(2.0, -ft / 96.0));

		for (note = 0; note < NOTES; note++)
			period_table[ft & 15][note] =
			    (note_period[note] * scale + 0x8000) >> 16;
	}
	for (i = 0; i < 32; i++)
		sine_table[i] = (int)floor(255 * sin(M_PI * i / 32));
	done = true;
}

static int
clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* The note a pattern's period names: the nearest, for files whose
 * periods stray from the table. */
static int
note_of(int period)
{
	int note, best = 0;

	for (note = 1; note < NOTES; note++)
		if (abs(note_period[note] - period) <
		    abs(note_period[best] - period))
			best = note;
	return best;
}

static bool
played(const struct replay *r, int order, int row)
{
	return (r->played[order][row / 8] & (1 << (row % 8))) != 0;
}

/*
 * Forget that a span of rows played.  A pattern loop replays its
 * section as part of one pass through the song, so rows left marked
 * would make the pass after the jump read as the song coming round.
 */
static void
unplay(struct replay *r, int order, int from, int to)
{
	int row;

	for (row = from; row <= to; row++)
		r->played[order][row / 8] &= ~(1 << (row % 8));
}

/* ── Effects ─────────────────────────────────────────────────── */

/* Arpeggio's note: the first in the channel's table at or below its
 * period, raised by some semitones. */
static int
arpeggio(const struct replay_chan *c, int semitones)
{
	const int *table = period_table[c->finetune & 15];
	int note;

	if (semitones == 0)
		return c->period;
	for (note = 0; note < NOTES - 1; note++)
		if (table[note] <= c->period)
			break;
	return table[clamp(note + semitones, 0, NOTES - 1)];
}

/* Slide toward the target note, stopping on it. */
static void
tone_porta(struct replay_chan *c)
{
	if (c->porta_target == 0)
		return;
	if (c->period < c->porta_target) {
		c->period += c->porta_speed;
		if (c->period > c->porta_target)
			c->period = c->porta_target;
	} else if (c->period > c->porta_target) {
		c->period -= c->porta_speed;
		if (c->period < c->porta_target)
			c->period = c->porta_target;
	}
	c->out_period = c->period;
}

/* The first half of the waveform raises the period, the second lowers
 * it; the depth is in 128ths of the table's range. */
static void
vibrato(struct replay_chan *c)
{
	int delta = sine_table[c->vib_pos & 31] * c->vib_depth >> 7;

	c->out_period = c->period + ((c->vib_pos & 32) ? -delta : delta);
	c->vib_pos = (c->vib_pos + c->vib_speed) & 63;
}

/* As vibrato, on the volume, with the depth in 64ths. */
static void
tremolo(struct replay_chan *c)
{
	int delta = sine_table[c->trem_pos & 31] * c->trem_depth >> 6;

	c->out_volume = clamp(c->volume +
	    ((c->trem_pos & 32) ? -delta : delta), 0, 64);
	c->trem_pos = (c->trem_pos + c->trem_speed) & 63;
}

/* Up by the high nibble, or if that is 0, down by the low one. */
static void
volume_slide(struct replay_chan *c)
{
	int up = c->param >> 4, down = c->param & 0xF;

	c->volume = clamp(up != 0 ? c->volume + up : c->volume - down, 0, 64);
	c->out_volume = c->volume;
}

/* The effects that act once, on a row's first tick. */
static void
row_effect(struct replay *r, struct replay_chan *c)
{
	int x = c->param >> 4, y = c->param & 0xF;

	switch (c->effect) {
	case 0x3:
		if (c->param != 0)
			c->porta_speed = c->param;
		break;
	case 0x4:
		if (x != 0)
			c->vib_speed = x;
		if (y != 0)
			c->vib_depth = y;
		break;
	case 0x7:
		if (x != 0)
			c->trem_speed = x;
		if (y != 0)
			c->trem_depth = y;
		break;
	case 0xB:
		r->jump_order = c->param;
		break;
	case 0xC:
		c->volume = clamp(c->param, 0, 64);
		break;
	case 0xD:
		/* The row is written in decimal. */
		r->break_row = x * 10 + y;
		if (r->break_row >= MOD_ROWS)
			r->break_row = 0;
		break;
	case 0xE:
		switch (x) {
		case 0xA:
			c->volume = clamp(c->volume + y, 0, 64);
			break;
		case 0xB:
			c->volume = clamp(c->volume - y, 0, 64);
			break;
		case 0x6:
			/*
			 * The count is a channel's own, so a module can
			 * loop one channel's section while the others play
			 * through; the jump it asks for is the song's.
			 */
			if (y == 0) {
				c->loop_row = r->row;
			} else {
				if (c->loop_count == 0)
					c->loop_count = y;
				else
					c->loop_count--;
				if (c->loop_count != 0)
					r->loop_to = c->loop_row;
			}
			break;
		case 0xE:
			if (r->row_delay == 0)
				r->row_delay = y;
			break;
		}
		break;
	case 0xF:
		if (c->param == 0)
			r->ended = true;
		else if (c->param < 0x20)
			r->speed = c->param;
		else
			r->bpm = c->param;
		break;
	}
}

/*
 * The effects that act on each tick of a row after the first — and on
 * every tick of a row that repeats, the first included.
 */
static void
tick_effect(struct replay *r, struct replay_chan *c, struct replay_voice *v)
{
	int x = c->param >> 4, y = c->param & 0xF;

	c->out_period = c->period;
	c->out_volume = c->volume;
	if (c->period == 0)
		return;

	switch (c->effect) {
	case 0x0:
		if (c->param != 0) {
			int semitones = 0;

			if (r->tick % 3 == 1)
				semitones = x;
			else if (r->tick % 3 == 2)
				semitones = y;
			c->out_period = arpeggio(c, semitones);
		}
		break;
	case 0x1:
		c->period -= c->param;
		if (c->period < PERIOD_MIN)
			c->period = PERIOD_MIN;
		c->out_period = c->period;
		break;
	case 0x2:
		c->period += c->param;
		if (c->period > PERIOD_MAX)
			c->period = PERIOD_MAX;
		c->out_period = c->period;
		break;
	case 0x3:
		tone_porta(c);
		break;
	case 0x4:
		vibrato(c);
		break;
	case 0x5:
		tone_porta(c);
		volume_slide(c);
		break;
	case 0x6:
		vibrato(c);
		volume_slide(c);
		break;
	case 0x7:
		tremolo(c);
		break;
	case 0xA:
		volume_slide(c);
		break;
	case 0xE:
		switch (x) {
		case 0x9:
			/* The sample restarts on every tick the count
			 * divides, so a count of 0 restarts nothing. */
			if (y != 0 && r->tick % y == 0)
				v->trigger = true;
			break;
		case 0xD:
			if (c->delay != 0 && c->delay == r->tick) {
				v->trigger = true;
				c->delay = 0;
			}
			break;
		}
		break;
	}
}

/*
 * Where a sample plays from: ProTracker plays a looping sample through
 * to the end of its loop first, or to the end of the sample when the
 * loop starts at 0, and then repeats the loop.  What follows the pass
 * is known whatever starts it; the first pass only matters to a start,
 * and an offset past it leaves nothing to play before the loop.
 */
static void
sample_regions(const struct mod_sample *smp, uint32_t offset, bool start,
    struct replay_voice *v)
{
	uint32_t first = smp->length;

	if (smp->loop_length != 0 && smp->loop_start != 0)
		first = smp->loop_start + smp->loop_length;

	v->loop.offset = smp->offset + smp->loop_start;
	v->loop.length = smp->loop_length;
	if (start && offset < first) {
		v->play.offset = smp->offset + offset;
		v->play.length = first - offset;
	}
}

/* ── Rows ────────────────────────────────────────────────────── */

static void
row_start(struct replay *r, struct replay_tick *t)
{
	const struct mod *m = r->mod;
	int pattern = m->order[r->order];
	int ch;

	r->played[r->order][r->row / 8] |= 1 << (r->row % 8);

	for (ch = 0; ch < MOD_CHANNELS; ch++) {
		const struct mod_note *n = mod_note(m, pattern, r->row, ch);
		struct replay_chan *c = &r->chan[ch];
		struct replay_voice *v = &t->voice[ch];
		bool retrigger = n->effect == 0xE &&
		    (n->param >> 4) == 0x9 && (n->param & 0xF) != 0;

		c->effect = n->effect;
		c->param = n->param;
		c->delay = 0;

		if (n->sample != 0) {
			const struct mod_sample *smp = &m->sample[n->sample];

			c->sample = n->sample;
			c->volume = smp->volume;
			c->finetune = smp->finetune;
		}

		if (n->period != 0 && c->sample != 0) {
			int period =
			    period_table[c->finetune & 15][note_of(n->period)];

			if (n->effect == 0x3 || n->effect == 0x5) {
				c->porta_target = period;
			} else if (n->effect == 0xE &&
			    (n->param >> 4) == 0xD && (n->param & 0xF) != 0) {
				/*
				 * A delayed note: ProTracker sets its period
				 * now, so what is playing bends to it, and
				 * restarts the sample on the tick named — or
				 * never, if the row ends first.
				 */
				c->period = period;
				c->delay = n->param & 0xF;
			} else {
				c->period = period;
				c->vib_pos = 0;
				c->trem_pos = 0;
				v->trigger = true;
				if (n->effect == 0x9) {
					if (n->param != 0)
						c->offset = (uint32_t)n->param << 8;
					c->trigger_offset = c->offset;
				}
			}
		}

		/*
		 * A retrigger on a row that starts no note restarts what
		 * the channel already plays, this tick included.  Where
		 * the row does start one, that start is the tick's
		 * retrigger.
		 */
		if (retrigger && !v->trigger &&
		    c->period != 0 && c->sample != 0)
			v->trigger = true;

		/*
		 * A sample number that starts nothing still changes the
		 * sample: ProTracker latches it into the channel's loop
		 * registers, so it takes over when the current pass ends.
		 */
		if (n->sample != 0 && !v->trigger)
			v->relatch = true;

		row_effect(r, c);
		c->out_period = c->period;
		c->out_volume = c->volume;
	}
}

static void
advance(struct replay *r)
{
	const struct mod *m = r->mod;

	if (++r->tick < r->speed)
		return;
	r->tick = 0;

	if (r->row_delay > 0) {
		r->row_delay--;
		r->repeating = true;
		return;
	}
	r->repeating = false;

	if (r->jump_order >= 0 || r->break_row >= 0) {
		r->order = r->jump_order >= 0 ? r->jump_order : r->order + 1;
		r->row = r->break_row >= 0 ? r->break_row : 0;
		r->jump_order = -1;
		r->break_row = -1;
	} else if (r->loop_to >= 0) {
		/* A jump within the order, so the song's position is
		 * where it was and only the row moves. */
		unplay(r, r->order, r->loop_to, r->row);
		r->row = r->loop_to;
	} else if (++r->row == MOD_ROWS) {
		r->row = 0;
		r->order++;
	}
	r->loop_to = -1;

	if (r->order >= m->length) {
		if (!r->loop) {
			r->ended = true;
			return;
		}
		r->order = 0;
	}

	/* A row played before means the song has come round. */
	if (!r->loop && played(r, r->order, r->row)) {
		r->loop_order = r->order;
		r->ended = true;
	}
}

void
replay_init(struct replay *r, const struct mod *m, int start, bool loop)
{
	tables_init();
	memset(r, 0, sizeof(*r));
	r->mod = m;
	r->loop = loop;
	r->order = start;
	r->speed = 6;
	r->bpm = 125;
	r->jump_order = -1;
	r->break_row = -1;
	r->loop_to = -1;
	r->loop_order = -1;
}

bool
replay_next(struct replay *r, struct replay_tick *t)
{
	int ch;

	if (r->ended)
		return false;

	memset(t, 0, sizeof(*t));
	/* Only a row's own 9xx starts anywhere but the sample's start. */
	for (ch = 0; ch < MOD_CHANNELS; ch++)
		r->chan[ch].trigger_offset = 0;
	if (r->tick == 0 && !r->repeating) {
		row_start(r, t);
		if (r->ended)		/* F00 stops the song on its row */
			return false;
	} else {
		for (ch = 0; ch < MOD_CHANNELS; ch++)
			tick_effect(r, &r->chan[ch], &t->voice[ch]);
	}

	t->order = r->order;
	t->pattern = r->mod->order[r->order];
	t->row = r->row;
	t->tick = r->tick;
	t->speed = r->speed;
	t->bpm = r->bpm;
	for (ch = 0; ch < MOD_CHANNELS; ch++) {
		const struct replay_chan *c = &r->chan[ch];
		struct replay_voice *v = &t->voice[ch];

		v->instrument = c->sample;
		v->period = c->out_period;
		v->volume = c->out_volume;
		if (c->sample != 0 && (v->trigger || v->relatch))
			sample_regions(&r->mod->sample[c->sample],
			    c->trigger_offset, v->trigger, v);
	}

	advance(r);
	return true;
}

/* ── Cues ────────────────────────────────────────────────────── */

/*
 * The order playback first moves on to from this one.  Returns -1 when
 * it moves on to none: the order comes round on itself, or the song
 * stops inside it.
 */
static int
order_exit(const struct mod *m, int order)
{
	struct replay r;
	struct replay_tick t;

	replay_init(&r, m, order, false);
	while (replay_next(&r, &t))
		if (t.order != order)
			return t.order;
	return -1;
}

/* Play a cue from its first order to where it comes round, for how far
 * it reaches and how long one pass lasts. */
static void
cue_walk(const struct mod *m, struct replay_cue *cue)
{
	uint8_t visited[MOD_ORDERS / 8];
	struct replay r;
	struct replay_tick t;

	memset(visited, 0, sizeof(visited));
	replay_init(&r, m, cue->start, false);
	cue->orders = 0;
	cue->us = 0;
	while (replay_next(&r, &t)) {
		if ((visited[t.order / 8] & 1 << (t.order % 8)) == 0) {
			visited[t.order / 8] |= 1 << (t.order % 8);
			cue->orders++;
		}
		cue->us += REPLAY_TICK_US((uint64_t)t.bpm);
	}
	cue->loop = r.loop_order;
}

int
replay_scan(const struct mod *m, struct replay_cue *cue, int max)
{
	int order, n = 0;

	for (order = 0; order < m->length && n < max; order++) {
		/* An order the one before it plays on into belongs to that
		 * cue rather than starting one. */
		if (order > 0 && order_exit(m, order - 1) == order)
			continue;
		cue[n].start = order;
		cue_walk(m, &cue[n]);
		n++;
	}
	return n;
}

const char *
replay_unsupported(const struct mod *m)
{
	static const char *const extended[16] = {
		NULL, "E1 fine porta up", "E2 fine porta down",
		"E3 glissando", "E4 vibrato waveform", "E5 finetune",
		NULL, "E7 tremolo waveform", "E8", NULL,
		NULL, NULL, "EC note cut", NULL, NULL, "EF invert loop",
	};
	static char list[512];
	uint32_t seen = 0;
	bool panning = false, filter = false;
	size_t len = 0;
	int o, row, ch, x;

	for (o = 0; o < m->length; o++)
		for (row = 0; row < MOD_ROWS; row++)
			for (ch = 0; ch < MOD_CHANNELS; ch++) {
				const struct mod_note *n =
				    mod_note(m, m->order[o], row, ch);

				/* E01 turns the Amiga's filter off, which
				 * is how this always sounds. */
				if (n->effect == 0x8)
					panning = true;
				else if (n->effect == 0xE && n->param == 0x00)
					filter = true;
				else if (n->effect == 0xE &&
				    extended[n->param >> 4] != NULL)
					seen |= 1U << (n->param >> 4);
			}

	if (!panning && !filter && seen == 0)
		return NULL;
	list[0] = '\0';
	if (panning)
		len += snprintf(list + len, sizeof(list) - len, "8 panning");
	if (filter)
		len += snprintf(list + len, sizeof(list) - len, "%sE00 filter",
		    len > 0 ? ", " : "");
	for (x = 0; x < 16; x++)
		if (seen & (1U << x))
			len += snprintf(list + len, sizeof(list) - len, "%s%s",
			    len > 0 ? ", " : "", extended[x]);
	return list;
}

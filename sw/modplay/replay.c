/*
 * The replayer's sequencing, the same whatever a song's format: which
 * order, row and tick comes next, the jumps a row's effects ask for,
 * and where the song comes round.  What a row's notes and effects mean
 * is the format's (song.h).
 */

#include <string.h>

#include "replay.h"

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

/* ── Rows ────────────────────────────────────────────────────── */

static void
advance(struct replay *r)
{
	const struct song *s = r->song;

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
	} else if (++r->row == SONG_ROWS) {
		r->row = 0;
		r->order++;
	}
	r->loop_to = -1;

	if (r->order >= s->length) {
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
replay_init(struct replay *r, const struct song *s, int start, bool loop)
{
	memset(r, 0, sizeof(*r));
	r->song = s;
	r->loop = loop;
	r->order = start;
	r->jump_order = -1;
	r->break_row = -1;
	r->loop_to = -1;
	r->loop_order = -1;
	s->format->start(r);
}

bool
replay_next(struct replay *r, struct replay_tick *t)
{
	bool row = r->tick == 0 && !r->repeating;

	if (r->ended)
		return false;

	memset(t, 0, sizeof(*t));
	if (row)
		r->played[r->order][r->row / 8] |= 1 << (r->row % 8);
	r->song->format->play(r, t, row);
	if (r->ended)		/* a row can stop the song on itself */
		return false;

	t->order = r->order;
	t->pattern = r->song->order[r->order];
	t->row = r->row;
	t->tick = r->tick;
	t->speed = r->speed;
	t->bpm = r->bpm;

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
order_exit(const struct song *s, int order)
{
	struct replay r;
	struct replay_tick t;

	replay_init(&r, s, order, false);
	while (replay_next(&r, &t))
		if (t.order != order)
			return t.order;
	return -1;
}

/* Play a cue from its first order to where it comes round, for how far
 * it reaches and how long one pass lasts. */
static void
cue_walk(const struct song *s, struct replay_cue *cue)
{
	uint8_t visited[SONG_ORDERS / 8];
	struct replay r;
	struct replay_tick t;

	memset(visited, 0, sizeof(visited));
	replay_init(&r, s, cue->start, false);
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
replay_scan(const struct song *s, struct replay_cue *cue, int max)
{
	int order, n = 0;

	for (order = 0; order < s->length && n < max; order++) {
		/* An order the one before it plays on into belongs to that
		 * cue rather than starting one. */
		if (order > 0 && order_exit(s, order - 1) == order)
			continue;
		cue[n].start = order;
		cue_walk(s, &cue[n]);
		n++;
	}
	return n;
}

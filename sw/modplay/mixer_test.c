/*
 * Host unit test for the software mixer (mixer.c).
 *
 *   make test            (from sw/modplay)
 *
 * The mixer's blocks skip the test for a pass's end wherever they can,
 * so it is checked against a mixer that takes every frame one at a time
 * and tests every one: over passes that end mid-block, loops shorter
 * than a step, loops latched mid-pass, starts with an empty first pass
 * and one-shots running out, with voices sharing pans and not, the two
 * must agree to the last bit, however the output is asked for.
 */

#include <stdio.h>
#include <string.h>

#include "mixer.h"

#define	MEMSIZE		4096
#define	RATE		24000
#define	VOICES		8
#define	TICKS		200
#define	TRIALS		40

static int8_t mem[MEMSIZE];
static int failures;
static uint32_t seed = 1;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

static uint32_t
rnd(uint32_t n)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 8) % n;
}

/* ── The reference: every frame on its own ───────────────────── */

struct ref_voice {
	bool		sounding;
	fix32_t		pos, inc;
	int32_t		end;
	struct replay_region loop;
	int		period, pan;
	int32_t		gain;
};

struct ref {
	fix32_t		clock_inc;
	int		crossfeed;
	struct ref_voice v[VOICES];
	uint32_t	pending;
	fix32_t		carry;
};

static void
ref_tick(struct ref *r, const struct replay_tick *t)
{
	fix32_t len;
	int k;

	for (k = 0; k < VOICES; k++) {
		const struct replay_voice *rv = &t->voice[k];
		struct ref_voice *v = &r->v[k];

		if (rv->instrument == 0 || rv->period == 0)
			continue;
		if (rv->trigger) {
			const struct replay_region *first =
			    rv->play.length != 0 ? &rv->play : &rv->loop;

			v->sounding = first->length != 0;
			if (!v->sounding)
				continue;
			v->pos = fix32_from_int((int32_t)first->offset);
			v->end = (int32_t)(first->offset + first->length);
			v->loop = rv->loop;
		} else if (rv->relatch && v->sounding)
			v->loop = rv->loop;
		v->inc = fix32_div_int(r->clock_inc, (uint32_t)rv->period);
		v->gain = rv->volume << 6;
		v->pan = rv->pan;
	}
	len = fix32_ratio(RATE * 5, 2 * (uint32_t)t->bpm) + r->carry;
	r->pending = (uint32_t)fix32_to_int(len);
	r->carry = fix32_frac(len);
}

static int16_t
clamp16(int32_t x)
{
	return x > INT16_MAX ? INT16_MAX : x < INT16_MIN ? INT16_MIN : x;
}

static void
ref_frame(struct ref *r, int16_t *out)
{
	int32_t sum[2 * REPLAY_PAN + 1];
	bool used[2 * REPLAY_PAN + 1];
	int32_t left = 0, right = 0;
	int k, p;

	memset(sum, 0, sizeof(sum));
	memset(used, 0, sizeof(used));
	for (k = 0; k < VOICES; k++) {
		struct ref_voice *v = &r->v[k];

		if (!v->sounding)
			continue;
		used[v->pan + REPLAY_PAN] = true;
		sum[v->pan + REPLAY_PAN] += mem[fix32_to_int(v->pos)] * v->gain;
		v->pos += v->inc;
		if (fix32_to_int(v->pos) < v->end)
			continue;
		if (v->loop.length == 0) {
			v->sounding = false;
			continue;
		}
		v->pos = fix32_from_int((int32_t)v->loop.offset) +
		    (v->pos - fix32_from_int(v->end));
		v->end = (int32_t)(v->loop.offset + v->loop.length);
		while (fix32_to_int(v->pos) >= v->end)
			v->pos -= fix32_from_int((int32_t)v->loop.length);
	}
	for (p = 0; p <= 2 * REPLAY_PAN; p++) {
		int32_t centre = REPLAY_PAN * (100 + r->crossfeed);
		int32_t whole = 2 * centre;
		int32_t spread = (p - REPLAY_PAN) * (100 - r->crossfeed);

		if (!used[p])
			continue;
		left += (sum[p] >> 5) *
		    ((256 * (centre - spread) + whole / 2) / whole);
		right += (sum[p] >> 5) *
		    ((256 * (centre + spread) + whole / 2) / whole);
	}
	out[0] = clamp16(left >> 8);
	out[1] = clamp16(right >> 8);
}

/* ── Scripts of ticks ────────────────────────────────────────── */

/* A region of sample memory, some of them tiny. */
static struct replay_region
region(void)
{
	struct replay_region r;

	r.length = rnd(4) == 0 ? 1 + rnd(3) : 1 + rnd(MEMSIZE / 2);
	r.offset = rnd(MEMSIZE - r.length + 1);
	return r;
}

/* A tick's worth of changes to each voice: starts, latched loops,
 * pitches from far below the rate to several frames a step. */
static void
script_tick(struct replay_tick *t, int tick, bool loud)
{
	int k;

	memset(t, 0, sizeof(*t));
	t->bpm = rnd(8) == 0 ? 32 + rnd(224) : 125;
	for (k = 0; k < VOICES; k++) {
		struct replay_voice *v = &t->voice[k];

		v->instrument = 1;
		v->period = rnd(6) == 0 ? 20 + rnd(80) : 100 + rnd(3000);
		v->volume = loud ? 64 : (int)rnd(65);
		v->pan = loud ? REPLAY_PAN : rnd(3) == 0 ?
		    (int)rnd(2 * REPLAY_PAN + 1) - REPLAY_PAN :
		    (k & 1 ? REPLAY_PAN / 2 : -REPLAY_PAN / 2);
		if (tick == 0 || rnd(12) == 0) {
			v->trigger = true;
			v->play = rnd(5) == 0 ? (struct replay_region){ 0, 0 } :
			    region();
			v->loop = rnd(4) == 0 ? (struct replay_region){ 0, 0 } :
			    region();
		} else if (rnd(10) == 0) {
			v->relatch = true;
			v->loop = rnd(4) == 0 ? (struct replay_region){ 0, 0 } :
			    region();
		}
	}
}

/*
 * Mix a script both ways, asking the mixer for its output in pieces
 * of the given size (0: random), and compare every frame.
 */
static bool
agrees(uint32_t trial_seed, uint32_t piece, bool loud, int crossfeed)
{
	static struct mixer m;
	static struct ref r;
	static int16_t got[4096 * 2];
	struct song song;
	struct replay_tick t;
	int16_t want[2];
	uint32_t n, i;
	int tick;

	memset(&song, 0, sizeof(song));
	song.channels = VOICES;
	song.clock = 3546895;
	song.sampledata = (const uint8_t *)mem;
	song.sampledata_size = MEMSIZE;
	mixer_init(&m, &song, RATE, crossfeed);
	memset(&r, 0, sizeof(r));
	r.clock_inc = fix32_ratio((int32_t)song.clock, RATE);
	r.crossfeed = crossfeed;

	seed = trial_seed;
	for (tick = 0; tick < TICKS; tick++) {
		script_tick(&t, tick, loud);
		mixer_tick(&m, &t);
		ref_tick(&r, &t);
		for (;;) {
			uint32_t max = piece != 0 ? piece : 1 + rnd(300);

			if ((n = mixer_mix(&m, got, max)) == 0)
				break;
			for (i = 0; i < n; i++) {
				if (r.pending == 0)
					return false;
				ref_frame(&r, want);
				r.pending--;
				if (got[2 * i] != want[0] ||
				    got[2 * i + 1] != want[1])
					return false;
			}
		}
		if (r.pending != 0)
			return false;
	}
	return true;
}

int
main(void)
{
	uint32_t trial, pieces[] = { 0, 1, 7, MIXER_BLOCK, 1000 };
	struct song song;
	struct replay_tick t;
	static struct mixer m;
	uint64_t frames = 0;
	size_t p;
	bool ok;
	int tick;

	for (trial = 0; trial < MEMSIZE; trial++)
		mem[trial] = (int8_t)rnd(256);

	for (p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
		char msg[96];

		ok = true;
		for (trial = 0; trial < TRIALS && ok; trial++)
			ok = agrees(0x5eed + trial, pieces[p], false,
			    (int)(trial % 101));
		snprintf(msg, sizeof(msg), "matches the frame-by-frame mixer, "
		    "asked for %s%u frames at a time",
		    pieces[p] == 0 ? "up to " : "", pieces[p] ? pieces[p] : 300);
		check(ok, msg);
	}

	ok = true;
	for (trial = 0; trial < 20 && ok; trial++)
		ok = agrees(0xc1a0 + trial, 0, true, 0);
	check(ok, "... and clamps loud voices the same way");

	/* A tick at 33 bpm is 1818 2/11 frames: the part-frames add up
	 * rather than drop, short only by what truncating each one's
	 * fraction loses, which comes to under a frame. */
	memset(&song, 0, sizeof(song));
	song.channels = 1;
	song.clock = 3546895;
	song.sampledata = (const uint8_t *)mem;
	song.sampledata_size = MEMSIZE;
	mixer_init(&m, &song, RATE, 0);
	memset(&t, 0, sizeof(t));
	t.bpm = 33;
	for (tick = 0; tick < 33 * 400; tick++) {
		mixer_tick(&m, &t);
		frames += m.pending;
		m.pending = 0;
	}
	check(frames == (uint64_t)RATE * 1000 ||
	    frames == (uint64_t)RATE * 1000 - 1,
	    "a tick's part-frames carry over: 1000 s of ticks at 33 bpm "
	    "are 1000 s of frames");

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

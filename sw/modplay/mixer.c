/*
 * The software mixer.
 *
 * Positions and steps are 32.32 fixed point (fixed.h), so a sample can
 * be as long as sample memory.  A voice mixes
 * a block at a time: when the pass in progress cannot end within the
 * block, the block runs with no test for its end at all, which for any
 * sample much longer than a block is nearly every block; otherwise it
 * runs frame by frame up to the end and on into what follows.
 *
 * Levels match the device's own mixing (modplay.c, voice_gain()): a
 * voice at full volume puts half of full scale into its two sides
 * together.
 */

#include <assert.h>
#include <string.h>

#include "mixer.h"

/*
 * A voice's gain is its volume (0..64) scaled up, so that a finer
 * volume can come later without changing the mix: a sample times a
 * gain fits in 20 bits, and REPLAY_VOICES of them sum without overflow.
 */
#define	GAIN_SHIFT	6

/* A bus sum shifted down this far, times a side's share in 256ths,
 * then down 8 more, is that side's output at full 16-bit scale. */
#define	BUS_SHIFT	5

static int16_t
clamp16(int32_t x)
{
	if (x > INT16_MAX)
		return INT16_MAX;
	if (x < INT16_MIN)
		return INT16_MIN;
	return (int16_t)x;
}

void
mixer_init(struct mixer *m, const struct song *s, uint32_t rate,
    int crossfeed)
{
	int32_t centre = REPLAY_PAN * (100 + crossfeed);
	int32_t whole = 2 * centre;
	int pan;

	/* Every frame of sample memory has to be a position. */
	assert(s->sampledata_size <= INT32_MAX);

	memset(m, 0, sizeof(*m));
	m->mem = (const int8_t *)s->sampledata;
	m->memsize = s->sampledata_size;
	m->rate = rate;
	m->clock_inc = fix32_ratio((int32_t)s->clock, rate);
	m->voices = s->channels;

	/* The shares voice_gain() gives the device's voices, so both
	 * sound alike. */
	for (pan = -REPLAY_PAN; pan <= REPLAY_PAN; pan++) {
		int32_t spread = pan * (100 - crossfeed);

		m->pan_left[pan + REPLAY_PAN] =
		    (256 * (centre - spread) + whole / 2) / whole;
		m->pan_right[pan + REPLAY_PAN] =
		    (256 * (centre + spread) + whole / 2) / whole;
	}
}

/* ── Voices ──────────────────────────────────────────────────── */

/* Regions come from the song's own sample table, so one outside
 * sample memory is a bug in its format, not in the file. */
static void
check_region(const struct mixer *m, const struct replay_region *r)
{
	assert(r->offset <= m->memsize && r->length <= m->memsize - r->offset);
}

/*
 * The pass in progress has ended: the position has reached or passed
 * end.  Go on into the loop that follows, the position carrying as far
 * past its start as it ran past the end, or fall silent if there is no
 * loop.  Returns false once the voice is silent; otherwise the position
 * is inside the new pass and end is where that pass ends.
 */
static bool
pass_end(struct mixer_voice *v, fix32_t *pos, int32_t *end)
{
	if (v->loop.length == 0) {
		return false;
	} else {
		fix32_t pos_remainder = *pos - fix32_from_int(*end);
		while (pos_remainder >= fix32_from_int(v->loop.length)) {
			pos_remainder -= fix32_from_int(v->loop.length);
		}
		*pos = fix32_from_int(v->loop.offset) + pos_remainder;
		*end = v->loop.offset + v->loop.length;
		return true;
	}
}

/* Whether the pass in progress might end within n frames, each of
 * which moves the position less than the step's whole frames plus one. */
static bool
pass_reachable(fix32_t pos, fix32_t inc, int32_t end, uint32_t n)
{
	return end - fix32_to_int(pos) <= (int64_t)n * (fix32_to_int(inc) + 1);
}

/* Mix n frames of a voice into a bus. */
static void
voice_mix(const int8_t *mem, struct mixer_voice *v, int32_t *acc,
    uint32_t n)
{
	fix32_t pos = v->pos, inc = v->inc;
	int32_t end = v->end, gain = v->gain;
	uint32_t f = 0;

	while (f < n) {
		if (!pass_reachable(pos, inc, end, n - f)) {
			for (; f < n; f++) {
				int8_t s = mem[fix32_to_int(pos)];

				pos += inc;
				acc[f] += s * gain;
			}
			break;
		}

		/* Frame by frame, up to the end of the pass. */
		do {
			int8_t s = mem[fix32_to_int(pos)];

			pos += inc;
			acc[f++] += s * gain;
		} while (f < n && fix32_to_int(pos) < end);

		if (fix32_to_int(pos) >= end && !pass_end(v, &pos, &end)) {
			v->sounding = false;
			break;
		}
	}
	v->pos = pos;
	v->end = end;
}

/* Start a voice's first pass, or its loop when the first pass is
 * empty; false when there is nothing to play at all. */
static bool
voice_start(const struct mixer *m, struct mixer_voice *v,
    const struct replay_voice *rv)
{
	const struct replay_region *first =
	    rv->play.length != 0 ? &rv->play : &rv->loop;

	check_region(m, &rv->play);
	check_region(m, &rv->loop);
	if (first->length == 0)
		return false;
	v->pos = fix32_from_int((int32_t)first->offset);
	v->end = (int32_t)(first->offset + first->length);
	v->loop = rv->loop;
	return true;
}

/* ── Ticks ───────────────────────────────────────────────────── */

void
mixer_tick(struct mixer *m, const struct replay_tick *t)
{
	fix32_t len;
	int k;

	/* As modplay.c's voice_event() tells the device's voices. */
	for (k = 0; k < m->voices; k++) {
		const struct replay_voice *rv = &t->voice[k];
		struct mixer_voice *v = &m->voice[k];

		if (rv->instrument == 0 || rv->period == 0)
			continue;
		if (rv->trigger) {
			v->sounding = voice_start(m, v, rv);
			if (!v->sounding)
				continue;
		} else if (rv->relatch && v->sounding) {
			check_region(m, &rv->loop);
			v->loop = rv->loop;
		}
		if (rv->period != v->period) {
			v->inc = fix32_div_int(m->clock_inc, (uint32_t)rv->period);
			v->period = rv->period;
		}
		v->gain = rv->volume << GAIN_SHIFT;
		v->pan = rv->pan;
	}

	/* A tick lasts 2.5 s / bpm: rate * 5 / (2 * bpm) frames, the part
	 * of a frame left over carried into the next. */
	assert(t->bpm > 0);
	len = fix32_ratio((int32_t)(m->rate * 5), 2 * (uint32_t)t->bpm) +
	    m->carry;
	m->pending = (uint32_t)fix32_to_int(len);
	m->carry = fix32_frac(len);
}

static void
mix_block(struct mixer *m, int16_t *out, uint32_t n)
{
	int32_t left[MIXER_BLOCK], right[MIXER_BLOCK];
	int bus_pan[REPLAY_VOICES];
	uint32_t f;
	int nbus = 0, k, b;

	for (k = 0; k < m->voices; k++) {
		struct mixer_voice *v = &m->voice[k];

		if (!v->sounding)
			continue;
		for (b = 0; b < nbus && bus_pan[b] != v->pan; b++)
			;
		if (b == nbus) {
			bus_pan[nbus++] = v->pan;
			memset(m->bus[b], 0, n * sizeof(m->bus[b][0]));
		}
		voice_mix(m->mem, v, m->bus[b], n);
	}

	memset(left, 0, n * sizeof(left[0]));
	memset(right, 0, n * sizeof(right[0]));
	for (b = 0; b < nbus; b++) {
		int32_t gl = m->pan_left[bus_pan[b] + REPLAY_PAN];
		int32_t gr = m->pan_right[bus_pan[b] + REPLAY_PAN];
		const int32_t *acc = m->bus[b];

		for (f = 0; f < n; f++) {
			int32_t x = acc[f] >> BUS_SHIFT;

			left[f] += x * gl;
			right[f] += x * gr;
		}
	}
	for (f = 0; f < n; f++) {
		out[2 * f] = clamp16(left[f] >> 8);
		out[2 * f + 1] = clamp16(right[f] >> 8);
	}
}

uint32_t
mixer_mix(struct mixer *m, int16_t *out, uint32_t max)
{
	uint32_t done = 0;

	while (done < max && m->pending > 0) {
		uint32_t n = m->pending;

		if (n > MIXER_BLOCK)
			n = MIXER_BLOCK;
		if (n > max - done)
			n = max - done;
		mix_block(m, out + 2 * done, n);
		done += n;
		m->pending -= n;
	}
	return done;
}

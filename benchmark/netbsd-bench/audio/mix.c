/* audio/mix.c - mixing tracker voices in software.
 *
 * The audio device mixes four channels in hardware, so a song with more
 * voices than that has to be mixed by the CPU into a single stream.  This
 * measures that mix, at the rate it would run and for the voice counts
 * real songs use, with its two loops either way round.
 *
 * Frame by frame, every voice is visited for every output frame, so each
 * voice's state is fetched from memory and written back once per sample.
 * Voice by voice, each voice is mixed across the whole block in turn, so
 * its state is fetched once, held in registers for the block, and written
 * back once.  Both do the same additions in a different order and so mix
 * the same output; what differs is how much of the loop is bookkeeping.
 *
 * A 16.16 position reaches only 65535 frames into a sample, which a
 * module's samples can outrun, so the wide methods mix voice by voice
 * with a 32.32 position: the frame in the high word, the fraction in the
 * low.  One tests for the loop end at every frame, as the 16.16 methods
 * do.  The other asks once per voice whether its block can reach the loop
 * end at all, and mixes the block with no test when it cannot, which for
 * any sample much longer than a block is nearly every time.
 *
 * One iteration mixes a millisecond of audio at MIX_HZ, so the ns an
 * iteration takes, divided by 10000, is the share of the CPU a mix
 * running continuously costs: 50000 ns per millisecond is 5%.
 *
 * Each voice loops its own sample, as a song's voices do, so the samples
 * stream through the cache alongside the mix.  Pitches spread over two
 * octaves of instruments tuned to TUNE_HZ, and every voice sits at its
 * own volume.
 */

#include "../bench.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIX_HZ        24000                 /* frames mixed per second */
#define FRAMES        (MIX_HZ / 1000)       /* one iteration: 1 ms */
#define MAX_VOICES    8
#define SAMPLE_BYTES  4096                  /* one voice's looping sample */
#define TUNE_HZ       8448                  /* middle C's sample rate */
#define OUT_FRAMES    256                   /* the ring the mix lands in */

struct voice {
    const int8_t *data;
    uint32_t len;                           /* frames, which are bytes */
    uint32_t pos;                           /* 16.16 */
    uint32_t inc;                           /* 16.16 source frames per frame */
    int vol;                                /* 0..64 */
    int side;                               /* 0 left, 1 right */
};

struct wide_voice {
    const int8_t *data;
    uint32_t len;                           /* frames, which are bytes */
    uint64_t pos;                           /* 32.32 */
    uint64_t inc;                           /* 32.32 source frames per frame */
    int vol;                                /* 0..64 */
    int side;                               /* 0 left, 1 right */
};

struct mix_ctx {
    struct voice v[MAX_VOICES];
    struct wide_voice w[MAX_VOICES];        /* the same voices, 32.32 */
    int nvoices;
    int16_t out[OUT_FRAMES * 2];
    uint32_t at;                            /* next frame in out */
};

static inline int16_t clamp16(int32_t x) {
    if (x > INT16_MAX)
        return INT16_MAX;
    if (x < INT16_MIN)
        return INT16_MIN;
    return (int16_t)x;
}

static inline void emit(struct mix_ctx *c, int32_t l, int32_t r) {
    int16_t *o = &c->out[(c->at++ & (OUT_FRAMES - 1)) * 2];

    o[0] = clamp16(l);
    o[1] = clamp16(r);
}

static void mix_by_frame(uint64_t iters, void *vctx) {
    struct mix_ctx *c = vctx;

    for (uint64_t i = 0; i < iters; i++)
        for (int f = 0; f < FRAMES; f++) {
            int32_t acc[2] = { 0, 0 };

            for (int k = 0; k < c->nvoices; k++) {
                struct voice *v = &c->v[k];
                int8_t s = v->data[v->pos >> 16];

                v->pos += v->inc;
                if ((v->pos >> 16) >= v->len)
                    v->pos -= v->len << 16;
                acc[v->side] += s * v->vol;
            }
            emit(c, acc[0], acc[1]);
        }
    bench_consume(c->out);
}

static void mix_by_voice(uint64_t iters, void *vctx) {
    struct mix_ctx *c = vctx;
    int32_t acc[2][FRAMES];

    for (uint64_t i = 0; i < iters; i++) {
        memset(acc, 0, sizeof(acc));
        for (int k = 0; k < c->nvoices; k++) {
            struct voice *v = &c->v[k];

            /* Copied out, since the accumulator may alias a voice's
             * fields as far as C is concerned, and a store through it
             * would otherwise force each one back to memory. */
            const int8_t *data = v->data;
            uint32_t pos = v->pos, inc = v->inc, len = v->len;
            int vol = v->vol;
            int32_t *a = acc[v->side];

            for (int f = 0; f < FRAMES; f++) {
                int8_t s = data[pos >> 16];

                pos += inc;
                if ((pos >> 16) >= len)
                    pos -= len << 16;
                a[f] += s * vol;
            }
            v->pos = pos;
        }
        for (int f = 0; f < FRAMES; f++)
            emit(c, acc[0][f], acc[1][f]);
    }
    bench_consume(c->out);
}

/* Mix one voice's block into a, testing for the loop end at every frame,
 * and return the position the block ends at.  The voice's fields arrive
 * as values, so a store to a cannot force them back to memory. */
static inline uint64_t wide_block_checked(int32_t *a, const int8_t *data,
                                          uint64_t pos, uint64_t inc,
                                          uint32_t len, int vol) {
    for (int f = 0; f < FRAMES; f++) {
        int8_t s = data[pos >> 32];

        pos += inc;
        if ((uint32_t)(pos >> 32) >= len)
            pos -= (uint64_t)len << 32;
        a[f] += s * vol;
    }
    return pos;
}

/* The same for a block that cannot reach the loop end. */
static inline uint64_t wide_block(int32_t *a, const int8_t *data,
                                  uint64_t pos, uint64_t inc, int vol) {
    for (int f = 0; f < FRAMES; f++) {
        int8_t s = data[pos >> 32];

        pos += inc;
        a[f] += s * vol;
    }
    return pos;
}

/* Whether a voice at pos, stepping inc, stays short of its loop end at
 * len for a whole block, so the block can skip the test.  Rounding the
 * step up to whole frames errs towards the checked loop, by at most
 * FRAMES frames; 32 bits hold it for any step below 2^32 / FRAMES. */
static inline bool wide_block_clear(uint64_t pos, uint64_t inc,
                                    uint32_t len) {
    return len - (uint32_t)(pos >> 32) > FRAMES * ((uint32_t)(inc >> 32) + 1);
}

static void mix_wide(uint64_t iters, void *vctx) {
    struct mix_ctx *c = vctx;
    int32_t acc[2][FRAMES];

    for (uint64_t i = 0; i < iters; i++) {
        memset(acc, 0, sizeof(acc));
        for (int k = 0; k < c->nvoices; k++) {
            struct wide_voice *v = &c->w[k];

            v->pos = wide_block_checked(acc[v->side], v->data, v->pos,
                                        v->inc, v->len, v->vol);
        }
        for (int f = 0; f < FRAMES; f++)
            emit(c, acc[0][f], acc[1][f]);
    }
    bench_consume(c->out);
}

static void mix_wide_hoisted(uint64_t iters, void *vctx) {
    struct mix_ctx *c = vctx;
    int32_t acc[2][FRAMES];

    for (uint64_t i = 0; i < iters; i++) {
        memset(acc, 0, sizeof(acc));
        for (int k = 0; k < c->nvoices; k++) {
            struct wide_voice *v = &c->w[k];

            if (wide_block_clear(v->pos, v->inc, v->len))
                v->pos = wide_block(acc[v->side], v->data, v->pos, v->inc,
                                    v->vol);
            else
                v->pos = wide_block_checked(acc[v->side], v->data, v->pos,
                                            v->inc, v->len, v->vol);
        }
        for (int f = 0; f < FRAMES; f++)
            emit(c, acc[0][f], acc[1][f]);
    }
    bench_consume(c->out);
}

/* Put every voice back where it starts, so each measurement mixes the
 * same material from the same point. */
static void voices_reset(struct mix_ctx *c, const int8_t *samples, int n) {
    /* Pitches in semitones from middle C, and a volume each. */
    static const int semis[MAX_VOICES] = { -12, 7, 0, 12, -5, 4, 16, -8 };
    static const int vols[MAX_VOICES] = { 64, 48, 56, 40, 60, 36, 52, 44 };

    c->nvoices = n;
    c->at = 0;
    for (int k = 0; k < n; k++) {
        struct voice *v = &c->v[k];
        struct wide_voice *w = &c->w[k];
        double ratio = (double)TUNE_HZ / MIX_HZ * pow(2.0, semis[k] / 12.0);

        v->data = samples + (size_t)k * SAMPLE_BYTES;
        v->len = SAMPLE_BYTES;
        v->pos = 0;
        v->inc = (uint32_t)lround(ratio * 65536.0);
        v->vol = vols[k];
        v->side = k & 1;

        /* The 16.16 step widened, not a finer one, so that every method
         * mixes the same output. */
        w->data = v->data;
        w->len = v->len;
        w->pos = 0;
        w->inc = (uint64_t)v->inc << 16;
        w->vol = v->vol;
        w->side = v->side;
    }
}

void bench_audio_mix(void) {
    static const int counts[] = { 4, 6, 8 };
    static const struct {
        const char *name;
        bench_fn f;
    } methods[] = {
        { "mix_by_frame", mix_by_frame },
        { "mix_by_voice", mix_by_voice },
        { "mix_wide", mix_wide },
        { "mix_wide_hoisted", mix_wide_hoisted },
    };
    static struct mix_ctx ctx;
    int8_t *samples = malloc((size_t)MAX_VOICES * SAMPLE_BYTES);
    uint32_t seed = 12345;

    if (samples == NULL) {
        perror("malloc");
        return;
    }

    /* Noise, so the sample reads spread the way real material does
     * rather than settling on a few values. */
    for (size_t i = 0; i < (size_t)MAX_VOICES * SAMPLE_BYTES; i++) {
        seed = seed * 1103515245u + 12345u;
        samples[i] = (int8_t)(seed >> 16);
    }

    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++)
        for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
            char cfg[16];

            voices_reset(&ctx, samples, counts[n]);
            snprintf(cfg, sizeof(cfg), "voices=%d", counts[n]);
            bench_time("audio", methods[m].name, cfg, methods[m].f, &ctx);
        }

    free(samples);
}

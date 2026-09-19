/*
 * rand.h — xorshift32, via rt.h.
 *
 * Nine ALU operations and a word of state, with a period of 2^32-1.
 * Each part keeps its own stream, so one part's draws cannot shift
 * another's, and a given seed replays exactly — the demo has to look
 * the same in the capture as on the machine.
 *
 * The outputs are linear over GF(2), which noise, dither and scatter
 * do not care about; a part that does can multiply a draw by
 * 0x2545F491, one MUL, to scramble it.
 */

#ifndef RAND_H
#define RAND_H

typedef uint32_t rt_rand_t;

/* Any seed works: the state is scrambled so that parts seeded with
 * neighbouring numbers do not start with lookalike streams. */
static inline void rt_rand_seed(rt_rand_t *r, uint32_t seed)
{
    seed = (seed ^ 0x9E3779B9u) * 0x85EBCA6Bu;
    seed ^= seed >> 16;
    *r = seed | 1u;                     /* zero is the one dead state */
}

static inline uint32_t rt_rand(rt_rand_t *r)
{
    uint32_t x = *r;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *r = x;
}

/* Below n, from the high half of a full product: one MUL, no divide,
 * and no modulo bias worth the cycles to remove. */
static inline uint32_t rt_rand_below(rt_rand_t *r, uint32_t n)
{
    return (uint32_t)(((uint64_t)rt_rand(r) * n) >> 32);
}

/* In [0, 1). */
static inline fix16_t rt_rand_frac(rt_rand_t *r)
{
    return (fix16_t)(rt_rand(r) >> 16);
}

/* In [-1, 1). */
static inline fix16_t rt_rand_signed(rt_rand_t *r)
{
    return (fix16_t)((int32_t)rt_rand(r) >> 15);
}

#endif /* RAND_H */

/*
 * fix16.h — signed 16.16 fixed point, via rt.h.
 *
 * Results truncate, and overflow wraps except in fix16_div(), which
 * saturates.  Costs are for penumbra2_5 at -O2.
 */

#ifndef FIX16_H
#define FIX16_H

typedef int32_t fix16_t;

#define FIX16_ONE       ((fix16_t)0x10000)
#define FIX16_HALF      ((fix16_t)0x8000)
#define FIX16_MAX       ((fix16_t)0x7FFFFFFF)
#define FIX16_MIN       ((fix16_t)0x80000000)

/* Constant expressions only: floating point anywhere else has no
 * library to link against, and the build fails. */
#define F16(x)  ((fix16_t)((x) * 65536.0 + ((x) < 0 ? -0.5 : 0.5)))

static inline fix16_t fix16_from_int(int32_t n)
{
    return (fix16_t)((uint32_t)n << 16);
}

/* Floor, so -0.5 gives -1. */
static inline int32_t fix16_to_int(fix16_t a)
{
    return a >> 16;
}

static inline int32_t fix16_round(fix16_t a)
{
    return (a + FIX16_HALF) >> 16;
}

/* Always in [0, 1): the floor's complement, including for negatives. */
static inline fix16_t fix16_frac(fix16_t a)
{
    return a & 0xFFFF;
}

/* One MUL, whose high half keeps the product's middle 32 bits. */
static inline fix16_t fix16_mul(fix16_t a, fix16_t b)
{
    return (fix16_t)(((int64_t)a * b) >> 16);
}

/* 2 DIVU for |b| < 1.0, 3 below 256.0, 5 below 4096.0: per span or
 * per vertex, never per pixel.  Division by zero traps. */
fix16_t fix16_div(fix16_t a, fix16_t b);

/* From a at t = 0 to b at t = FIX16_ONE. */
static inline fix16_t fix16_lerp(fix16_t a, fix16_t b, fix16_t t)
{
    return a + fix16_mul(b - a, t);
}

#endif /* FIX16_H */

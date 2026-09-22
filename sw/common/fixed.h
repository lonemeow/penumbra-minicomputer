/*
 * Fixed-point arithmetic: a signed value carrying a whole part and a
 * fraction.  In 16.16, a 32-bit value spans about ±32768 to a
 * resolution of one 65536th; in 32.32, a 64-bit one spans an int32_t's
 * range to a resolution of one 2^32th.
 *
 * Carrying a quantity this way keeps the part of each step that a
 * whole number would round away, so something advanced in steps too
 * small to show still moves instead of standing where it was.
 *
 * A type is named for how many of its bits are fraction, so a further
 * format joins these as fix24_t or fix20_t, with operations of its
 * own: what a format is chosen for decides both how wide its whole
 * part must be and which operations it needs.
 *
 * Add and subtract with + and -, keeping the result in range.  The
 * 16.16 operations work in 64 bits, so no intermediate overflows, and
 * a result too large for the type comes back at the end of the range
 * it ran past rather than wrapping.  32.32 has nothing wider to work
 * in, so it has only operations that cannot overflow for the arguments
 * they take.  All of them round down, toward negative infinity, so a
 * value stepped repeatedly moves the same way on either side of zero.
 */

#ifndef PENUMBRA_FIXED_H
#define PENUMBRA_FIXED_H

#include <stdint.h>

/* Whole division rounding down rather than toward zero, which is what
 * a shift does and so what the rest of these agree with. */
static inline int64_t
fix_divfloor(int64_t n, int64_t d)
{
	int64_t q = n / d;

	if (n % d != 0 && (n < 0) != (d < 0))
		q--;
	return q;
}

typedef int32_t fix16_t;

#define	FIX16_SHIFT	16
#define	FIX16_ONE	((fix16_t)1 << FIX16_SHIFT)

/* The whole numbers at the ends of the range. */
#define	FIX16_INT_MAX	(INT32_MAX >> FIX16_SHIFT)
#define	FIX16_INT_MIN	(INT32_MIN >> FIX16_SHIFT)

/* A wide result brought back to the type, held at the end it ran past. */
static inline fix16_t
fix16_narrow(int64_t v)
{
	if (v > INT32_MAX)
		return INT32_MAX;
	if (v < INT32_MIN)
		return INT32_MIN;
	return (fix16_t)v;
}

static inline fix16_t
fix16_from_int(int v)
{
	return (fix16_t)v * FIX16_ONE;
}

/* The whole part; the fraction is a remainder the value goes on
 * carrying, not something to round on the way out. */
static inline int
fix16_to_int(fix16_t f)
{
	return (int)(f >> FIX16_SHIFT);
}

static inline fix16_t
fix16_mul(fix16_t a, fix16_t b)
{
	return fix16_narrow(((int64_t)a * b) >> FIX16_SHIFT);
}

/* The divisor must not be zero. */
static inline fix16_t
fix16_div(fix16_t a, fix16_t b)
{
	return fix16_narrow(fix_divfloor((int64_t)a * FIX16_ONE, b));
}

/*
 * Scale by a ratio of whole numbers — a rate over an interval, a span
 * across a width.  Neither a multiply nor a divide alone does this:
 * the product needs the range between them that only the wide
 * intermediate has.
 */
static inline fix16_t
fix16_scale(fix16_t f, int64_t num, int64_t den)
{
	return fix16_narrow(fix_divfloor((int64_t)f * num, den));
}

static inline fix16_t
fix16_clamp(fix16_t f, fix16_t lo, fix16_t hi)
{
	if (f < lo)
		return lo;
	if (f > hi)
		return hi;
	return f;
}

/*
 * 32.32 counts what outgrows 16.16's whole part while still moving in
 * steps far finer than one: a position in memory, and how far it
 * advances at each step.
 */
typedef int64_t fix32_t;

#define	FIX32_SHIFT	32
#define	FIX32_ONE	((fix32_t)1 << FIX32_SHIFT)

static inline fix32_t
fix32_from_int(int32_t v)
{
	return (fix32_t)v * FIX32_ONE;
}

/* The whole part, as fix16_to_int() takes it. */
static inline int32_t
fix32_to_int(fix32_t f)
{
	return (int32_t)(f >> FIX32_SHIFT);
}

/* What the whole part leaves: the fraction, from 0 up to one. */
static inline fix32_t
fix32_frac(fix32_t f)
{
	return f & (FIX32_ONE - 1);
}

/* A ratio of whole numbers, which the whole part always holds.  The
 * divisor must not be zero. */
static inline fix32_t
fix32_ratio(int32_t num, uint32_t den)
{
	return fix_divfloor((int64_t)num * FIX32_ONE, den);
}

/* A value divided by a whole number, which must not be zero. */
static inline fix32_t
fix32_div_int(fix32_t f, uint32_t n)
{
	return fix_divfloor(f, n);
}

#endif /* PENUMBRA_FIXED_H */

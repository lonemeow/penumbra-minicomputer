/*
 * Fixed-point arithmetic in 16.16: a signed 32-bit value carrying a
 * whole part and sixteen bits of fraction, spanning about ±32768 to a
 * resolution of one 65536th.
 *
 * Carrying a quantity this way keeps the part of each step that a
 * whole number would round away, so something advanced in steps too
 * small to show still moves instead of standing where it was.
 *
 * A type is named for how many of its bits are fraction, so a second
 * format joins this one as fix24_t or fix20_t, with operations of its
 * own: what a format is chosen for decides both how wide its whole
 * part must be and which operations it needs.
 *
 * Add and subtract with + and -, keeping the result in range.  The
 * operations here work in 64 bits, so no intermediate overflows, and a
 * result too large for the type comes back at the end of the range it
 * ran past rather than wrapping.  All of them round down, toward
 * negative infinity, so a value stepped repeatedly moves the same way
 * on either side of zero.
 */

#ifndef PENUMBRA_FIXED_H
#define PENUMBRA_FIXED_H

#include <stdint.h>

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

/* Whole division rounding down rather than toward zero, which is what
 * a shift does and so what the rest of these agree with. */
static inline int64_t
fix16_divfloor(int64_t n, int64_t d)
{
	int64_t q = n / d;

	if (n % d != 0 && (n < 0) != (d < 0))
		q--;
	return q;
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
	return fix16_narrow(fix16_divfloor((int64_t)a * FIX16_ONE, b));
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
	return fix16_narrow(fix16_divfloor((int64_t)f * num, den));
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

#endif /* PENUMBRA_FIXED_H */

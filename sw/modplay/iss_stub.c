/*
 * The little of the C library the tests need beyond what the ISS
 * harness (test/compiler/harness) carries, for running them on the
 * machine itself.
 *
 * Most of it is the handful of libm calls the replayers work their
 * tables out with.  The tables round to whole periods and to a sine of
 * 0..255, and no case reads a table's own values — the mixer and the
 * mixer it is held against read the same ones — so series this short
 * are close enough here.  They are not a libm, and nothing but these
 * tests should reach for them.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define	LN2	0.693147180559945309417

char *
strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);

	for (; *haystack != '\0'; haystack++)
		if (strncmp(haystack, needle, n) == 0)
			return (char *)haystack;
	return n == 0 ? (char *)haystack : NULL;
}

double
floor(double x)
{
	double whole = (double)(long long)x;

	return whole > x ? whole - 1.0 : whole;
}

long
lrint(double x)
{
	return (long)(x < 0 ? x - 0.5 : x + 0.5);
}

/* Folded onto the nearest quarter turn, where the series for sine and
 * cosine are at their shortest. */
double
sin(double x)
{
	double turns = x / (M_PI / 2);
	int quarter = (int)(turns < 0 ? turns - 0.5 : turns + 0.5);
	double t = x - quarter * (M_PI / 2), t2, s, c;

	t2 = t * t;
	s = t * (1 - t2 / 6 * (1 - t2 / 20 * (1 - t2 / 42)));
	c = 1 - t2 / 2 * (1 - t2 / 12 * (1 - t2 / 30));
	switch (quarter & 3) {
	case 0:
		return s;
	case 1:
		return c;
	case 2:
		return -s;
	default:
		return -c;
	}
}

double
cos(double x)
{
	return sin(x + M_PI / 2);
}

/*
 * Two raised to a power: the whole part by doubling and the fraction
 * from the series for e to the x.  Only two is ever raised here, so
 * another base is a mistake rather than something to approximate.
 */
double
pow(double base, double exponent)
{
	int whole = (int)(exponent < 0 ? exponent - 0.5 : exponent + 0.5);
	double t, r;

	if (base != 2.0)
		abort();
	t = (exponent - whole) * LN2;
	r = 1 + t * (1 + t / 2 * (1 + t / 3 * (1 + t / 4 * (1 + t / 5))));
	for (; whole > 0; whole--)
		r *= 2;
	for (; whole < 0; whole++)
		r /= 2;
	return r;
}

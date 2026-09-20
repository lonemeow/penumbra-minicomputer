/*
 * Host unit test for the fixed-point helpers (fixed.h).
 *
 *   make test            (from sw/common)
 *
 * The cases that matter are the two things carrying a value in 16.16
 * is for: a step too small to show as a whole number still moves the
 * value, and forming that step cannot overflow on the way.  Both are
 * checked at the extremes a ProTracker module actually reaches, since
 * a tick is as short as 2.5 s / 255 bpm.
 */

#include <stdbool.h>
#include <stdio.h>

#include "fixed.h"

#define	BPM_MIN		32	/* Fxx sets the tempo from 0x20 ... */
#define	BPM_MAX		255	/*  ... to 0xFF */
#define	TICK_US(bpm)	(2500000 / (bpm))

#define	METER_MAX	64	/* a bar's full scale, in whole steps */

static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

int
main(void)
{
	fix16_t full = fix16_from_int(METER_MAX);
	int64_t wide;
	int bpm, worst_fall_s = 0;
	bool every_step_moves = true;

	check(fix16_to_int(fix16_from_int(64)) == 64,
	    "a whole number survives the round trip");
	check(fix16_to_int(FIX16_ONE - 1) == 0,
	    "a fraction below one truncates to nothing");
	check(fix16_to_int(2 * FIX16_ONE - 1) == 1,
	    "and truncates rather than rounds above it");
	check(FIX16_INT_MAX >= METER_MAX,
	    "a bar's full scale is within the range a fix16_t holds");

	/*
	 * A bar's full scale times the shortest tick is about 4.1e10 and
	 * overflows 32 bits, so what this asks is that the helper forms
	 * the product as widely as working it out by hand would.
	 */
	wide = (int64_t)full * TICK_US(BPM_MAX) / 1000000;
	check(fix16_scale(full, TICK_US(BPM_MAX), 1000000) == wide,
	    "a product too wide for 32 bits is formed in 64");

	/*
	 * A bar that falls over ten seconds still moves on every tick of
	 * the fastest tempo a module can ask for.  Carried as a whole
	 * number 0..64 this is where the drop would round to zero and
	 * the bar would hang.
	 */
	for (bpm = BPM_MIN; bpm <= BPM_MAX; bpm++)
		if (fix16_scale(full, TICK_US(bpm), 10 * 1000000) <= 0)
			every_step_moves = false;
	check(every_step_moves,
	    "a ten-second fall still moves at every tempo a module sets");

	/* How slow a fall can get before a tick stops moving it. */
	while (fix16_scale(full, TICK_US(BPM_MAX), (int64_t)(worst_fall_s + 1)
	    * 1000000) > 0)
		worst_fall_s++;
	printf("     (a fall stops moving past %d s)\n", worst_fall_s);
	check(worst_fall_s > 600,
	    "no fall worth choosing comes near standing still");

	check(fix16_mul(fix16_from_int(3), fix16_from_int(4)) ==
	    fix16_from_int(12) &&
	    fix16_mul(FIX16_ONE / 2, FIX16_ONE / 2) == FIX16_ONE / 4,
	    "multiplying keeps the scale");
	check(fix16_div(fix16_from_int(12), fix16_from_int(4)) ==
	    fix16_from_int(3) &&
	    fix16_div(FIX16_ONE, fix16_from_int(2)) == FIX16_ONE / 2,
	    "dividing keeps the scale");

	/* A third of minus one lands below it, not above: the shift the
	 * other operations use rounds that way, so the divide does too. */
	check(fix16_div(fix16_from_int(-1), fix16_from_int(3)) == -21846 &&
	    fix16_scale(fix16_from_int(-1), 1, 3) == -21846 &&
	    fix16_to_int(-FIX16_ONE / 2) == -1,
	    "a negative result rounds down rather than toward zero");

	check(fix16_mul(fix16_from_int(30000), fix16_from_int(30000)) ==
	    INT32_MAX &&
	    fix16_mul(fix16_from_int(-30000), fix16_from_int(30000)) ==
	    INT32_MIN,
	    "a product past the range is held at its end, not wrapped");

	check(fix16_clamp(-1, 0, full) == 0 && fix16_clamp(full + 1, 0, full)
	    == full && fix16_clamp(FIX16_ONE, 0, full) == FIX16_ONE,
	    "clamping holds a value between its ends");

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

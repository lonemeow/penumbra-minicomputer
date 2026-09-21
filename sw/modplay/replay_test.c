/*
 * Host unit test for the replayer (replay.c).
 *
 *   make test            (from sw/modplay)
 *
 * The module under test is assembled here rather than read from a
 * file, so each case states the pattern data it depends on: a cue that
 * comes round on itself, one that spans several orders and comes round
 * to the middle of itself, and the two ways a retrigger falls within a
 * row.  It goes through mod_parse() all the same, so the layout the
 * parser expects is part of what is checked.
 */

#include <stdio.h>
#include <string.h>

#include "mod.h"
#include "replay.h"

#define	TEST_PATTERNS	4
#define	TEST_ORDERS	4
#define	SAMPLE_BYTES	64

/* Where mod.c reads each part of the file. */
#define	OFF_SAMPLES	20
#define	SAMPLE_HEADER	30
#define	OFF_LENGTH	950
#define	OFF_ORDERS	952
#define	OFF_SIGNATURE	1080
#define	OFF_PATTERNS	1084

/* The defaults a song starts at, which this module never changes. */
#define	TEST_SPEED	6
#define	TEST_TICK_US	REPLAY_TICK_US(125)

static uint8_t image[OFF_PATTERNS + TEST_PATTERNS * 1024 + SAMPLE_BYTES];
static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

static void
put_note(int pattern, int row, int ch, int period, int sample, int effect,
    int param)
{
	uint8_t *c = &image[OFF_PATTERNS +
	    ((pattern * MOD_ROWS + row) * MOD_CHANNELS + ch) * 4];

	c[0] = (uint8_t)((sample & 0xF0) | ((period >> 8) & 0x0F));
	c[1] = (uint8_t)period;
	c[2] = (uint8_t)(((sample << 4) & 0xF0) | (effect & 0x0F));
	c[3] = (uint8_t)param;
}

static const char *
build(struct mod *m)
{
	uint8_t *smp = &image[OFF_SAMPLES];
	int i;

	memset(image, 0, sizeof(image));
	memcpy(image, "replay test", 11);

	/* Sample 1, which every note here plays: a one-shot, at full
	 * volume, its length in words as the format stores it. */
	smp[22] = 0;
	smp[23] = SAMPLE_BYTES / 2;
	smp[25] = 64;

	image[OFF_LENGTH] = TEST_ORDERS;
	for (i = 0; i < TEST_ORDERS; i++)
		image[OFF_ORDERS + i] = (uint8_t)i;
	memcpy(&image[OFF_SIGNATURE], "M.K.", 4);

	/*
	 * The first cue: one order, its last row jumping back to itself.
	 * Its first row starts a note and retriggers it three ticks
	 * apart, and the row after starts none and retriggers two apart.
	 */
	put_note(0, 0, 0, 428, 1, 0xE, 0x93);
	put_note(0, 1, 0, 0, 0, 0xE, 0x92);
	put_note(0, MOD_ROWS - 1, 0, 0, 0, 0xB, 0x00);

	/* The second: orders 1 to 3, coming round to order 2. */
	put_note(3, MOD_ROWS - 1, 0, 0, 0, 0xB, 0x02);

	return mod_parse(m, image, sizeof(image));
}

/*
 * A second module, for the pattern loop: one order whose rows 1 to 3
 * are a loop section played three times — E60 marks the return row and
 * E62 asks for two further passes — after which the song plays out its
 * remaining rows and stops.
 */
#define	LOOP_START_ROW	1
#define	LOOP_END_ROW	3
#define	LOOP_PASSES	3

static const char *
build_loop(struct mod *m)
{
	uint8_t *smp = &image[OFF_SAMPLES];

	memset(image, 0, sizeof(image));
	memcpy(image, "pattern loop", 12);
	smp[23] = SAMPLE_BYTES / 2;
	smp[25] = 64;

	image[OFF_LENGTH] = 1;
	image[OFF_ORDERS] = 0;
	memcpy(&image[OFF_SIGNATURE], "M.K.", 4);

	put_note(0, LOOP_START_ROW, 0, 0, 0, 0xE, 0x60);
	put_note(0, LOOP_END_ROW, 0, 0, 0, 0xE, 0x62);

	return mod_parse(m, image, sizeof(image));
}

/* Times a row is entered in one pass through the song. */
static int
row_plays(const struct mod *m, int order, int row)
{
	struct replay r;
	struct replay_tick t;
	int n = 0;

	replay_init(&r, m, 0, false);
	while (replay_next(&r, &t))
		if (t.order == order && t.row == row && t.tick == 0)
			n++;
	return n;
}

/* The ticks within one row that channel 0 starts its sample on, as a
 * bitmap, playing the order the row belongs to. */
static unsigned
triggers(const struct mod *m, int order, int row)
{
	struct replay r;
	struct replay_tick t;
	unsigned mask = 0;

	replay_init(&r, m, order, false);
	while (replay_next(&r, &t)) {
		if (t.order != order)
			break;
		if (t.row == row && t.voice[0].trigger)
			mask |= 1U << t.tick;
	}
	return mask;
}

int
main(void)
{
	struct replay_cue cue[TEST_ORDERS];
	struct replay_tick t;
	struct replay r;
	struct mod m;
	const char *error;
	int n;

	if ((error = build(&m)) != NULL) {
		printf("FAIL the test module does not parse: %s\n", error);
		return 1;
	}

	check(replay_unsupported(&m) == NULL,
	    "a module whose only extended effect is E9 has nothing ignored");

	check(triggers(&m, 0, 0) == ((1U << 0) | (1U << 3)),
	    "E93 on a row that starts a note retriggers on tick 3");
	check(triggers(&m, 0, 1) == ((1U << 0) | (1U << 2) | (1U << 4)),
	    "E92 on a row that starts none retriggers from tick 0");

	replay_init(&r, &m, 2, false);
	check(replay_next(&r, &t) && t.order == 2,
	    "replay_init starts at the order it is given");

	n = replay_scan(&m, cue, TEST_ORDERS);
	check(n == 2, "the module holds two cues");
	if (n == 2) {
		check(cue[0].start == 0 && cue[0].orders == 1 &&
		    cue[0].loop == 0,
		    "the first is one order coming round on itself");
		check(cue[0].us ==
		    (uint64_t)MOD_ROWS * TEST_SPEED * TEST_TICK_US,
		    "... and lasts its 64 rows");
		check(cue[1].start == 1 && cue[1].orders == 3 &&
		    cue[1].loop == 2,
		    "the second spans three orders and comes round to the "
		    "middle one");
		check(cue[1].us ==
		    (uint64_t)3 * MOD_ROWS * TEST_SPEED * TEST_TICK_US,
		    "... and lasts all three orders' rows");
	}

	mod_free(&m);

	if ((error = build_loop(&m)) != NULL) {
		printf("FAIL the loop module does not parse: %s\n", error);
		return 1;
	}

	check(replay_unsupported(&m) == NULL,
	    "E6 is not reported as an effect the replayer ignores");
	check(row_plays(&m, 0, LOOP_START_ROW) == LOOP_PASSES,
	    "E62 plays its section three times");
	check(row_plays(&m, 0, LOOP_START_ROW - 1) == 1,
	    "the row before the section plays once");

	/*
	 * The rows of a loop section play more than once within a single
	 * pass, so a replayer that took a replayed row as the song
	 * coming round would stop at the first jump and never reach
	 * this row at all.
	 */
	check(row_plays(&m, 0, LOOP_END_ROW + 1) == 1,
	    "the song plays on past the section once the passes run out");

	n = replay_scan(&m, cue, TEST_ORDERS);
	check(n == 1 && cue[0].us == (uint64_t)(MOD_ROWS +
	    (LOOP_PASSES - 1) * (LOOP_END_ROW - LOOP_START_ROW + 1)) *
	    TEST_SPEED * TEST_TICK_US,
	    "... and lasts its rows plus the section's repeats");

	mod_free(&m);
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

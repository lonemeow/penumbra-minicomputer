/*
 * Host unit test for the displays (display.c and its backends).
 *
 *   make test            (from sw/modplay)
 *
 * The meters backend draws off-screen — display_start() with fd -1 —
 * and the frame is read back through the screen layer, so no terminal
 * is involved.  The module is fabricated rather than parsed: a display
 * reads only a title, a length and sample names out of one.
 *
 * What is asserted is the layout's contract and not the bars' motion:
 * how fast a bar falls is a tuning decision, so the cases below hold
 * whatever it is — a channel that sounds shows something, a channel
 * that does not shows nothing, and no bar leaves its track.
 */

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "screen.h"

/* Rows and columns are found by what they contain, so the cases
 * survive the layout being rearranged. */
#define	ROW_MAX		64
#define	TEXT_MAX	256

static struct mod mod;
static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

static void
build(void)
{
	memset(&mod, 0, sizeof(mod));
	strcpy(mod.title, "meter test");
	strcpy(mod.signature, "M.K.");
	mod.length = 4;
	strcpy(mod.sample[1].name, "leadline");
	mod.sample[1].volume = 64;
}

/* The first row whose text contains `want`, or -1. */
static int
row_with(const char *want)
{
	char buf[TEXT_MAX];
	int y;

	for (y = 0; y < scr_rows() && y < ROW_MAX; y++) {
		scr_row_text(y, buf, sizeof(buf));
		if (strstr(buf, want) != NULL)
			return y;
	}
	return -1;
}

/*
 * True when the cell at (row,x) belongs to a bar.  A filled cell takes
 * its colour in the background, the character set having no solid
 * glyph to take it in the foreground; an empty one carries `track`.
 */
static int
in_bar(int row, int x, scr_attr track)
{
	scr_attr a = scr_cell_attr(row, x);

	return a == track || ((a >> 4) & 0xf) != C_DEFAULT;
}

/*
 * A bar is the row's run of bar cells.  Returns its width, and the
 * count of filled cells through *filled.
 */
static int
bar_width(int row, int *filled)
{
	const scr_attr track = ATTR(C_BLUE, C_DEFAULT, 0);
	int x, start, n = 0;

	*filled = 0;
	if (row < 0)
		return -1;
	for (start = 0; start < scr_cols() && !in_bar(row, start, track);
	    start++)
		;
	for (x = start; x < scr_cols() && in_bar(row, x, track); x++, n++)
		if (scr_cell_attr(row, x) != track)
			(*filled)++;
	return n;
}

/* One tick: channel 0 plays sample 1, and nothing else sounds. */
static struct display_tick
tick_at(uint64_t us, int row, bool trigger, int volume)
{
	struct display_tick dt;

	memset(&dt, 0, sizeof(dt));
	dt.us = us;
	dt.tick.row = row;
	dt.tick.speed = 6;
	dt.tick.bpm = 125;
	dt.tick.voice[0].sample = 1;
	dt.tick.voice[0].period = 428;
	dt.tick.voice[0].volume = volume;
	dt.tick.voice[0].trigger = trigger;
	return dt;
}

int
main(void)
{
	struct display_tick dt;
	int sounding, silent, width, filled, quiet;

	build();

	check(display_select("line"), "the line display can be selected");
	check(display_select("meters"), "the meters display can be selected");
	check(!display_select("nonesuch"), "an unknown display is refused");

	if (!display_start(&mod, "test", -1)) {
		printf("FAIL the meters display would not start\n");
		return 1;
	}

	/* A note starts at full volume on the first tick. */
	dt = tick_at(0, 0, true, 64);
	display_update(&dt);

	check(row_with("meter test") >= 0, "the title row carries the module");
	sounding = row_with("leadline");
	check(sounding >= 0, "a sounding channel's row names its sample");

	width = bar_width(sounding, &filled);
	check(width > 0, "the sounding channel has a bar track");
	check(filled > 0, "a note that just started fills some of it");
	check(filled <= width, "a bar never runs past its track");

	/* A channel with nothing on it keeps an empty track. */
	silent = sounding + 1;
	check(bar_width(silent, &quiet) > 0 && quiet == 0,
	    "a channel that never sounds stays empty");

	check(row_with("order") >= 0 && row_with("125 bpm") >= 0,
	    "the status row carries the position and the tempo");

	/* Ticks keep arriving after the note; nothing may exceed the
	 * track however the bar is carried forward. */
	dt = tick_at(200000, 4, false, 64);
	display_update(&dt);
	width = bar_width(row_with("leadline"), &filled);
	check(filled <= width, "a bar stays within its track as it falls");

	display_end();
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

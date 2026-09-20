/*
 * The meter display: a bar per channel, the way trackers show one.
 *
 * Nothing reads back what the device is playing, so the bars are not
 * measured from the sound.  They are the score's own account of it: a
 * note start drives its channel's bar to the volume that note plays
 * at, and the bar falls from there.  What the eye follows is the
 * pattern being played rather than the waveform coming out, which is
 * what a tracker's own meters showed too.
 *
 * The player hands over every tick, far more often than a serial
 * console can be redrawn, so the bars are carried forward on each one
 * and painted on a pace of their own.
 */

#include <err.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "fixed.h"
#include "screen.h"

#define	METER_MAX	64	/* a bar reads 0..64, as a volume does */
#define	REDRAW_US	50000	/* 20 frames a second at most */

/* The layout: a title, a rule, a row per channel, a rule, the position. */
#define	ROW_TITLE	0
#define	ROW_CHAN	2
#define	ROW_STATUS	(ROW_CHAN + MOD_CHANNELS + 1)
#define	ROWS_MIN	(ROW_STATUS + 1)

#define	COL_NAME	4
#define	NAME_W		20
#define	COL_BAR		(COL_NAME + NAME_W + 1)

/*
 * The position line, not the bars, sets the narrowest screen the
 * meters draw on: it carries the time, the place in the song, the
 * speed and the tempo, at the widest each of those reads.  Both
 * consoles this runs on are 80 columns.
 */
#define	BAR_MIN		16	/* a bar narrower than this says little */
#define	STATUS_W	66
#define	COLS_MIN	(COL_BAR + BAR_MIN > STATUS_W ? COL_BAR + BAR_MIN \
			    : STATUS_W)

static const struct mod *mod;
static fix16_t meter[MOD_CHANNELS];
static uint64_t last_us, next_draw_us;
static char title[64];

/*
 * Paula puts channels 0 and 3 on the left and 1 and 2 on the right, so
 * a bar is coloured by the side it will be heard from.
 */
static int
channel_color(int ch)
{
	return (ch == 0 || ch == 3) ? C_CYAN : C_GREEN;
}

/*
 * Where a channel's bar stands after this tick: us microseconds have
 * passed since the one before, v says whether a note started on it and
 * how loud the channel is, and the answer spans 0 to METER_MAX whole
 * steps, carried finely enough that a fall never rounds to nothing.
 */
static fix16_t
meter_next(fix16_t level, const struct replay_voice *v, uint64_t us)
{
	if (v->trigger || fix16_from_int(v->volume) > level) {
		return fix16_from_int(v->volume);
	} else {
		return fix16_scale(level, (1000000 - us), 1000000);
	}
}

static void
draw(const struct display_tick *dt)
{
	const struct replay_tick *t = &dt->tick;
	unsigned s = (unsigned)(dt->us / 1000000);
	int width, ch, filled, i;

	width = scr_cols() - COL_BAR - 1;
	if (width > METER_MAX)
		width = METER_MAX;

	scr_clear();
	scr_puts(ROW_TITLE, 0, ATTR(C_WHITE, C_DEFAULT, 1), title);
	scr_fill(ROW_TITLE + 1, 0, scr_cols(), GLYPH_HLINE,
	    ATTR(C_BLUE, C_DEFAULT, 0));

	for (ch = 0; ch < MOD_CHANNELS; ch++) {
		const struct replay_voice *v = &t->voice[ch];
		int row = ROW_CHAN + ch;
		scr_attr bar = ATTR_SOLID(channel_color(ch));
		scr_attr track = ATTR(C_BLUE, C_DEFAULT, 0);

		scr_printf(row, 0, ATTR(C_DEFAULT, C_DEFAULT, 0), "%2d", ch + 1);
		scr_printf(row, COL_NAME, ATTR(C_YELLOW, C_DEFAULT, 0),
		    "%-*.*s", NAME_W, NAME_W,
		    v->sample != 0 ? mod->sample[v->sample].name : "");

		filled = fix16_to_int(fix16_scale(meter[ch], width,
		    METER_MAX));
		for (i = 0; i < width; i++)
			scr_putc(row, COL_BAR + i,
			    i < filled ? ' ' : GLYPH_SHADE,
			    i < filled ? bar : track);
	}

	scr_fill(ROW_STATUS - 1, 0, scr_cols(), GLYPH_HLINE,
	    ATTR(C_BLUE, C_DEFAULT, 0));
	scr_printf(ROW_STATUS, 0, ATTR(C_DEFAULT, C_DEFAULT, 0),
	    "%2u:%02u   order %2d/%d   pattern %2d   row %2d   "
	    "speed %d   %d bpm",
	    s / 60, s % 60, t->order, mod->length, t->pattern, t->row,
	    t->speed, t->bpm);
	scr_flush();
}

static bool
meters_start(const struct mod *m, const char *path, int fd)
{
	bool started;

	mod = m;
	memset(meter, 0, sizeof(meter));
	last_us = next_draw_us = 0;
	snprintf(title, sizeof(title), "modplay — %s", m->title);

	if (fd >= 0)
		started = scr_init_on(fd) == 0;
	else
		started = scr_init_mem(ROWS_MIN + 2, COLS_MIN + 8) == 0;
	if (!started) {
		warnx("%s: no terminal to draw the meters on", path);
		return false;
	}
	if (scr_rows() < ROWS_MIN || scr_cols() < COLS_MIN) {
		int r = scr_rows(), c = scr_cols();

		scr_shutdown();
		warnx("%s: the meters need %dx%d and the terminal is %dx%d",
		    path, COLS_MIN, ROWS_MIN, c, r);
		return false;
	}
	return true;
}

static void
meters_update(const struct display_tick *dt)
{
	uint64_t tick_us = REPLAY_TICK_US((uint64_t)dt->tick.bpm);
	uint64_t us = dt->us > last_us ? dt->us - last_us : 0;
	int ch;

	/*
	 * Ticks arrive one at a time, so a gap wider than one means the
	 * player fell far enough behind to drop some rather than that
	 * the song spent that long here.  The bars carry on from where
	 * they stand instead of lurching by the whole wait.
	 */
	if (us > tick_us)
		us = tick_us;

	for (ch = 0; ch < MOD_CHANNELS; ch++)
		meter[ch] = meter_next(meter[ch], &dt->tick.voice[ch], us);
	last_us = dt->us;

	if (dt->us < next_draw_us)
		return;
	next_draw_us = dt->us + REDRAW_US;
	draw(dt);
}

static void
meters_end(void)
{
	scr_shutdown();
}

const struct display_backend display_meters = {
	.name = "meters",
	.summary = "a bar per channel, as a tracker shows them",
	.start = meters_start,
	.update = meters_update,
	.end = meters_end,
};

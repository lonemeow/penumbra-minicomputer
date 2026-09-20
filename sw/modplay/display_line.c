/*
 * The line display: the song position, rewritten in place once a row,
 * since the console is a serial line and every character costs.
 */

#include <stdio.h>
#include <unistd.h>

#include "display.h"

static const struct mod *mod;
static int out_fd = STDOUT_FILENO;
static int shown_order = -1, shown_row = -1;

static bool
line_start(const struct mod *m, const char *path, int fd)
{
	mod = m;
	out_fd = fd;
	shown_order = shown_row = -1;
	dprintf(out_fd, "%s: \"%s\"\n", path, m->title);
	return true;
}

static void
line_update(const struct display_tick *dt)
{
	const struct replay_tick *t = &dt->tick;
	unsigned s = (unsigned)(dt->us / 1000000);

	if (t->order == shown_order && t->row == shown_row)
		return;
	shown_order = t->order;
	shown_row = t->row;

	dprintf(out_fd, "\r  %2u:%02u  order %2d/%d  pattern %2d  row %2d ",
	    s / 60, s % 60, t->order, mod->length, t->pattern, t->row);
}

static void
line_end(void)
{
	dprintf(out_fd, "\n");
}

const struct display_backend display_line = {
	.name = "line",
	.summary = "one line, rewritten in place",
	.start = line_start,
	.update = line_update,
	.end = line_end,
};

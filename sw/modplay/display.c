/*
 * A one-line display: the song position, rewritten in place once a
 * row, since the console is a serial line and every character costs.
 */

#include <stdio.h>

#include "display.h"

static const struct mod *mod;
static int shown_order = -1, shown_row = -1;

void
display_start(const struct mod *m, const char *path)
{
	mod = m;
	shown_order = shown_row = -1;
	printf("%s: \"%s\"\n", path, m->title);
}

void
display_update(const struct display_tick *dt)
{
	const struct replay_tick *t = &dt->tick;
	unsigned s = (unsigned)(dt->us / 1000000);

	if (t->order == shown_order && t->row == shown_row)
		return;
	shown_order = t->order;
	shown_row = t->row;

	printf("\r  %2u:%02u  order %2d/%d  pattern %2d  row %2d ",
	    s / 60, s % 60, t->order, mod->length, t->pattern, t->row);
	fflush(stdout);
}

void
display_end(void)
{
	putchar('\n');
}

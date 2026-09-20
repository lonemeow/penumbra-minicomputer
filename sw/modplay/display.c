/*
 * Choosing which display shows a song, and handing it each tick.
 */

#include <string.h>

#include "display.h"

extern const struct display_backend display_line;
extern const struct display_backend display_meters;

const struct display_backend *const display_backends[] = {
	&display_line,
	&display_meters,
	NULL,
};

static const struct display_backend *chosen = &display_line;

bool
display_select(const char *name)
{
	int i;

	for (i = 0; display_backends[i] != NULL; i++)
		if (strcmp(display_backends[i]->name, name) == 0) {
			chosen = display_backends[i];
			return true;
		}
	return false;
}

bool
display_start(const struct mod *m, const char *path, int fd)
{
	return chosen->start(m, path, fd);
}

void
display_update(const struct display_tick *dt)
{
	chosen->update(dt);
}

void
display_end(void)
{
	chosen->end();
}

/*
 * What modplay shows while a song plays.
 *
 * The player hands the display each tick as the device's clock reaches
 * it, as the replayer described it — every channel's sample, period,
 * volume and note starts, not only the song position — so a display
 * can show whatever the song is doing, and can read the module itself
 * for anything more, such as the pattern around the row.
 *
 * Each way of showing that is a backend in a file of its own, named in
 * the table display.c holds.  One that draws pixels rather than
 * characters is another entry there, not a change to the others.
 */

#ifndef MODPLAY_DISPLAY_H
#define MODPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "mod.h"
#include "replay.h"

struct display_tick {
	struct replay_tick tick;
	uint64_t	us;		/* song time at the tick's start */
};

struct display_backend {
	const char	*name;		/* what -d selects it by */
	const char	*summary;	/* one line, for the usage text */

	/*
	 * Fd is the terminal to draw on, or -1 to draw off-screen at a
	 * fixed size, which is how the host test reads a frame back.
	 * Returns false when the backend cannot draw there — too small a
	 * screen, no terminal at all — having said why, since the player
	 * stops rather than quietly showing something else.
	 */
	bool		(*start)(const struct mod *, const char *path, int fd);

	/* Called once per tick, and given them in order: a backend that
	 * cannot redraw that often holds what it needs and paces
	 * itself. */
	void		(*update)(const struct display_tick *);

	void		(*end)(void);
};

/* Every backend, NULL-terminated; the first is the default. */
extern const struct display_backend *const display_backends[];

/* False when no backend goes by that name. */
bool		 display_select(const char *name);

bool		 display_start(const struct mod *, const char *path, int fd);
void		 display_update(const struct display_tick *);
void		 display_end(void);

#endif /* MODPLAY_DISPLAY_H */

/*
 * What modplay shows while a song plays.
 *
 * The player hands the display each tick as the device's clock reaches
 * it, as the replayer described it — every channel's sample, period,
 * volume and note starts, not only the song position — so a display
 * can show whatever the song is doing, and can read the module itself
 * for anything more, such as the pattern around the row.
 */

#ifndef MODPLAY_DISPLAY_H
#define MODPLAY_DISPLAY_H

#include <stdint.h>

#include "mod.h"
#include "replay.h"

struct display_tick {
	struct replay_tick tick;
	uint64_t	us;		/* song time at the tick's start */
};

void	display_start(const struct mod *, const char *path);

/* Called whenever the player looks at the clock, often more than once
 * a tick; the display decides what is worth redrawing. */
void	display_update(const struct display_tick *);

void	display_end(void);

#endif /* MODPLAY_DISPLAY_H */

/*
 * A song, whichever format it came in: what the replayer and the
 * player need to know about it, and the formats that can supply one.
 *
 * A format parses its files into a song and gives the replayer what it
 * needs to play one: its own per-channel state and the meaning of a
 * row's notes and effects.  Everything else — orders, rows, ticks, the
 * jumps between them — the replayer does the same for every format.
 */

#ifndef MODPLAY_SONG_H
#define MODPLAY_SONG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mod.h"
#include "s3m.h"

#define	SONG_ORDERS	256
#define	SONG_ROWS	64	/* in every pattern */

struct replay;
struct replay_tick;
struct song;

struct song_format {
	/* Whether a file carries the format's signature; parse() then
	 * finds out whether it really is one. */
	bool		(*probe)(const uint8_t *, size_t);
	/* Fill in a song song_parse() has cleared.  Returns NULL, or what
	 * is wrong with the file.  The buffer must outlive the song. */
	const char	*(*parse)(struct song *, const uint8_t *, size_t);
	void		(*free)(struct song *);
	/* The effects the song uses that the format's replayer ignores,
	 * as a list of their names, or NULL. */
	const char	*(*unsupported)(const struct song *);

	/* Set up the replayer's state for the format, once the replayer
	 * has cleared it. */
	void		(*start)(struct replay *);
	/* One tick: a row's notes and first-tick effects when row is set,
	 * else the effects of a later tick, leaving what every channel
	 * plays in the tick's voices. */
	void		(*play)(struct replay *, struct replay_tick *, bool row);
};

extern const struct song_format mod_format;
extern const struct song_format s3m_format;

struct song {
	const struct song_format *format;
	char		title[29];
	char		kind[5];	/* what the file says it is */
	int		channels;	/* voices it plays */
	int		length;		/* orders the song plays */
	int		npatterns;
	uint8_t		order[SONG_ORDERS];	/* the pattern each plays */

	/* A voice plays at this rate divided by its period. */
	uint32_t	clock;

	/* The crossfeed, in percent, that suits the format's panning. */
	int		crossfeed;

	/* The samples back to back, signed 8-bit. */
	const uint8_t	*sampledata;
	uint32_t	sampledata_size;

	union {
		struct mod	mod;
		struct s3m	s3m;
	};
};

/* Copy a fixed-width text field, stopping at its first NUL and keeping
 * the result printable. */
void		 song_text(char *dst, const uint8_t *src, size_t n);

/* Parse a file in whichever format it is in.  Returns NULL, or what is
 * wrong with the file.  The buffer must outlive the song. */
const char	*song_parse(struct song *, const uint8_t *, size_t);
void		 song_free(struct song *);
const char	*song_unsupported(const struct song *);

#endif /* MODPLAY_SONG_H */

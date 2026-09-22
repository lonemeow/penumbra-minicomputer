/*
 * ScreamTracker 3 modules: the samples a file holds, the rows its
 * patterns pack, and the settings its header carries.
 */

#ifndef MODPLAY_S3M_H
#define MODPLAY_S3M_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define	S3M_CHANNELS	32	/* the file's, before unused ones are dropped */
#define	S3M_SAMPLES	99
#define	S3M_ROWS	64
#define	S3M_ORDERS	256

/* Scream Tracker plays a sample at this rate divided by its period. */
#define	S3M_CLOCK	14317056

/* The file sets each channel's place in the stereo field, so nothing
 * has to be spread for it. */
#define	S3M_CROSSFEED	0

/* A row's cell says what one channel does; a file leaves out whichever
 * parts of it the row does not set. */
#define	S3M_NOTE_CUT	254	/* ^^: stop what is playing */
#define	S3M_NOTE_NONE	255
#define	S3M_VOLUME_NONE	255

struct s3m_sample {
	char		name[29];
	uint32_t	offset;		/* bytes into the sample data */
	uint32_t	length;		/* bytes */
	uint32_t	loop_start;	/* bytes */
	uint32_t	loop_length;	/* bytes; 0 = does not loop */
	int		volume;		/* 0..64 */
	uint32_t	c2spd;		/* the rate middle C plays at */
};

struct s3m_cell {
	uint8_t		note;		/* octave << 4 | semitone */
	uint8_t		instrument;	/* 1..99; 0 = none */
	uint8_t		volume;		/* 0..64 */
	uint8_t		command;	/* 1 = A ... 26 = Z; 0 = none */
	uint8_t		info;
};

/* What a file can hold that modplay cannot play. */
#define	S3M_HAS_ADLIB	0x01
#define	S3M_HAS_PACKED	0x02
#define	S3M_HAS_16BIT	0x04
#define	S3M_HAS_STEREO	0x08

struct s3m {
	struct s3m_sample sample[S3M_SAMPLES + 1];	/* by number; [0] unused */
	int		nsamples;
	struct s3m_cell	*cell;		/* [npatterns][S3M_ROWS][channels] */
	int		channels;	/* the ones the song plays */
	int		unplayable;	/* S3M_HAS_* the file turned out to hold */

	/* The voice playing the channel a packed row names, or -1 where
	 * the song passes over that channel. */
	int		voice_of[S3M_CHANNELS];

	int		flags;		/* the header's */
	int		cwt;		/* the tracker version that wrote it */
	int		global_volume;	/* 0..64 */
	int		master_volume;	/* 0..127 */
	int		speed;		/* ticks per row, to start at */
	int		tempo;		/* and the tempo */
	int		pan[S3M_CHANNELS];	/* by voice */

	/* Where each of the file's orders ended up, since a Bxx names one
	 * of those: an order the song skips maps to the next it plays. */
	uint8_t		order_of[S3M_ORDERS];
};

/*
 * Scream Tracker's volume slides run on every tick, the first
 * included, for files this old or marked this way.
 */
#define	S3M_FLAG_FAST_SLIDES	0x40
#define	S3M_CWT_FAST_SLIDES	0x1300

static inline bool
s3m_fast_slides(const struct s3m *m)
{
	return (m->flags & S3M_FLAG_FAST_SLIDES) != 0 ||
	    m->cwt == S3M_CWT_FAST_SLIDES;
}

static inline const struct s3m_cell *
s3m_cell(const struct s3m *m, int pattern, int row, int ch)
{
	return &m->cell[(pattern * S3M_ROWS + row) * m->channels + ch];
}

#endif /* MODPLAY_S3M_H */

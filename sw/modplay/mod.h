/*
 * ProTracker modules: the 31-sample, four-channel format under its
 * common signatures.
 */

#ifndef MODPLAY_MOD_H
#define MODPLAY_MOD_H

#include <stddef.h>
#include <stdint.h>

#define	MOD_CHANNELS	4
#define	MOD_SAMPLES	31
#define	MOD_ROWS	64
#define	MOD_ORDERS	128

struct mod_sample {
	char		name[23];
	uint32_t	offset;		/* bytes into the sample data */
	uint32_t	length;		/* bytes */
	uint32_t	loop_start;	/* bytes */
	uint32_t	loop_length;	/* bytes; 0 = does not loop */
	int		finetune;	/* -8..7, eighths of a semitone */
	int		volume;		/* 0..64 */
};

struct mod_note {
	uint16_t	period;		/* 0 = no note */
	uint8_t		sample;		/* 1..31; 0 = none */
	uint8_t		effect;		/* 0x0..0xF */
	uint8_t		param;
};

struct mod {
	char		title[21];
	char		signature[5];
	struct mod_sample sample[MOD_SAMPLES + 1];	/* by number; [0] unused */
	uint8_t		order[MOD_ORDERS];
	int		length;		/* orders the song plays */
	int		npatterns;
	struct mod_note	*pattern;	/* [npatterns][MOD_ROWS][MOD_CHANNELS] */

	/* The samples back to back, as the file stores them; points into
	 * the buffer mod_parse() was given. */
	const uint8_t	*sampledata;
	uint32_t	sampledata_size;
};

/* Returns NULL, or what is wrong with the file.  The buffer must
 * outlive the module. */
const char	*mod_parse(struct mod *, const uint8_t *, size_t);
void		 mod_free(struct mod *);

static inline const struct mod_note *
mod_note(const struct mod *m, int pattern, int row, int ch)
{
	return &m->pattern[(pattern * MOD_ROWS + row) * MOD_CHANNELS + ch];
}

#endif /* MODPLAY_MOD_H */

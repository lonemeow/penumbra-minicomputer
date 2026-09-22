/*
 * Host unit test for the ScreamTracker 3 parser (s3m.c).
 *
 *   make test            (from sw/modplay)
 *
 * The module under test is assembled here rather than read from a
 * file, so each case states what it depends on: channels an OPL chip
 * would play or the file has switched off, orders that skip and end,
 * panning both from the file's table and from a channel's own side,
 * unsigned samples, a loop running past its sample, an instrument that
 * is not sound at all, and rows packed as the format packs them.
 */

#include <stdio.h>
#include <string.h>

#include "replay.h"

#define	ORDNUM		6
#define	INSNUM		3
#define	PATNUM		2

/* Where each part of the image goes; instruments follow the header's
 * lists, and a parapointer counts sixteenths. */
#define	OFF_ORDERS	0x60
#define	OFF_INS_PARA	(OFF_ORDERS + ORDNUM)
#define	OFF_PAT_PARA	(OFF_INS_PARA + 2 * INSNUM)
#define	OFF_PAN		(OFF_PAT_PARA + 2 * PATNUM)
#define	INS_AT		0xC0	/* three headers of 0x50 */
#define	PAT_AT		0x1B0
#define	SAMPLE_AT	0x200
#define	SAMPLE_BYTES	8

static uint8_t image[SAMPLE_AT + SAMPLE_BYTES];
static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

static void
put16(size_t at, uint32_t v)
{
	image[at] = (uint8_t)v;
	image[at + 1] = (uint8_t)(v >> 8);
}

static void
put32(size_t at, uint32_t v)
{
	put16(at, v & 0xFFFF);
	put16(at + 2, v >> 16);
}

/* One instrument header: a sample of its own, or something else the
 * type says. */
static void
instrument(int n, int type, uint32_t data_at, uint32_t length,
    uint32_t loop_start, uint32_t loop_end, int flags, int volume)
{
	size_t at = INS_AT + (size_t)(n - 1) * 0x50;

	put16(OFF_INS_PARA + 2 * (n - 1), (uint32_t)(at / 16));
	image[at] = (uint8_t)type;
	image[at + 0x0D] = (uint8_t)(data_at / 16 >> 16);
	put16(at + 0x0E, data_at / 16 & 0xFFFF);
	put32(at + 0x10, length);
	put32(at + 0x14, loop_start);
	put32(at + 0x18, loop_end);
	image[at + 0x1C] = (uint8_t)volume;
	image[at + 0x1F] = (uint8_t)flags;
	put32(at + 0x20, 8363);
	memcpy(&image[at + 0x30], "sample", 6);
	memcpy(&image[at + 0x4C], "SCRS", 4);
}

/*
 * Pattern 0, packed as the format packs it: a channel byte saying
 * which of note, volume and command follow, and a zero to end a row.
 * Channel 2 is the OPL one and channel 3 is switched off, so what they
 * play here should reach no voice at all.
 */
static void
pack_pattern(void)
{
	size_t at = PAT_AT + 2, start = at;

	image[at++] = 0 | 32 | 64 | 128;	/* channel 0, all three */
	image[at++] = 0x41;			/* C# of octave 4 */
	image[at++] = 1;
	image[at++] = 32;
	image[at++] = 1;			/* A: set speed */
	image[at++] = 6;
	image[at++] = 2 | 32;			/* the OPL channel */
	image[at++] = 0x30;
	image[at++] = 1;
	image[at++] = 3 | 64;			/* the channel that is off */
	image[at++] = 17;
	image[at++] = 4 | 32;			/* channel 4: the third voice */
	image[at++] = 0x30;
	image[at++] = 1;
	image[at++] = 0;			/* end of row 0 */
	image[at++] = 1 | 64;			/* row 1, channel 1 */
	image[at++] = 48;
	image[at++] = 0;			/* end of row 1 */
	put16(PAT_AT, (uint32_t)(at - start + 2));
	put16(OFF_PAT_PARA, PAT_AT / 16);
}

static const char *
build(struct song *song)
{
	static const uint8_t settings[] = { 0, 8, 16, 1 | 0x80, 9 };
	static const uint8_t sample[SAMPLE_BYTES] = {
		0x00, 0x40, 0x80, 0xC0, 0xFF, 0x80, 0x80, 0x80,
	};
	size_t i;

	memset(image, 0, sizeof(image));
	memcpy(image, "s3m test", 8);
	image[0x1C] = 0x1A;
	image[0x1D] = 16;
	put16(0x20, ORDNUM);
	put16(0x22, INSNUM);
	put16(0x24, PATNUM);
	put16(0x26, 0x40);		/* the volume slides of ST3.00 */
	put16(0x28, 0x1320);
	put16(0x2A, 2);			/* unsigned samples */
	memcpy(&image[0x2C], "SCRM", 4);
	image[0x30] = 64;		/* global volume */
	image[0x31] = 0;		/* no speed: 6 */
	image[0x32] = 20;		/* a tempo too low to run: 125 */
	image[0x33] = 0x80 | 48;	/* stereo, and the master volume */
	image[0x35] = 252;		/* the file sets the panning */

	memset(&image[0x40], 255, 32);
	for (i = 0; i < sizeof(settings); i++)
		image[0x40 + i] = settings[i];

	image[OFF_ORDERS + 0] = 0;
	image[OFF_ORDERS + 1] = 254;	/* skipped */
	image[OFF_ORDERS + 2] = 1;
	image[OFF_ORDERS + 3] = 5;	/* a pattern the file does not hold */
	image[OFF_ORDERS + 4] = 255;	/* the end */
	image[OFF_ORDERS + 5] = 0;	/* past it */

	/* The first two voices are panned by the file, the third by the
	 * side its channel setting puts it on. */
	image[OFF_PAN + 0] = 0x20 | 4;
	image[OFF_PAN + 1] = 0x20 | 11;
	image[OFF_PAN + 4] = 0;

	/* A looping sample whose loop runs past its end, an instrument an
	 * OPL chip would play, and a 16-bit sample. */
	instrument(1, 1, SAMPLE_AT, SAMPLE_BYTES, 2, 100, 0x01, 70);
	instrument(2, 2, 0, 0, 0, 0, 0, 64);
	instrument(3, 1, SAMPLE_AT, SAMPLE_BYTES, 0, 0, 0x04, 64);
	memcpy(&image[SAMPLE_AT], sample, SAMPLE_BYTES);

	pack_pattern();
	memset(song, 0, sizeof(*song));
	if (!s3m_format.probe(image, sizeof(image)))
		return "the file is not taken for an S3M";
	song->format = &s3m_format;
	return s3m_format.parse(song, image, sizeof(image));
}

int
main(void)
{
	struct song song;
	const struct s3m *m = &song.s3m;
	const char *error;

	if ((error = build(&song)) != NULL) {
		printf("FAIL the test module does not parse: %s\n", error);
		return 1;
	}

	check(strcmp(song.title, "s3m test") == 0 &&
	    strcmp(song.kind, "S3M") == 0 && song.clock == S3M_CLOCK,
	    "the header names the song and the clock its periods count");
	check(m->speed == 6 && m->tempo == 125 && m->global_volume == 64 &&
	    m->master_volume == 48,
	    "a speed and a tempo it cannot run at fall back to 6 and 125");
	check(s3m_fast_slides(m),
	    "the header asks for the volume slides of ST3.00");

	check(song.channels == 3,
	    "the channels an OPL chip would play, and those switched off, "
	    "are not voices");
	check((m->unplayable & S3M_HAS_ADLIB) != 0 &&
	    (m->unplayable & S3M_HAS_16BIT) != 0,
	    "... and what the file holds that modplay cannot play is known");
	check(m->voice_of[0] == 0 && m->voice_of[1] == 1 &&
	    m->voice_of[2] == -1 && m->voice_of[3] == -1 &&
	    m->voice_of[4] == 2,
	    "the voices are the channels that are left, in the file's order");
	check(m->pan[0] == -30 && m->pan[1] == 30 && m->pan[2] == 38,
	    "panning comes from the file's table, or from a channel's own "
	    "side where the table leaves it");

	check(song.length == 2 && song.order[0] == 0 && song.order[1] == 1,
	    "an order to skip, one naming no pattern, and everything past "
	    "the end are not played");
	check(m->order_of[0] == 0 && m->order_of[1] == 1 &&
	    m->order_of[2] == 1 && m->order_of[3] == 2,
	    "... but a jump still finds an order by its place in the file");

	check(m->sample[1].length == SAMPLE_BYTES &&
	    m->sample[1].volume == 64 && m->sample[1].c2spd == 8363,
	    "a sample's length and rate are read, and its volume held to 64");
	check(m->sample[1].loop_start == 2 && m->sample[1].loop_length == 6,
	    "a loop running past its sample ends with the sample");
	check(m->sample[2].length == 0 && m->sample[3].length == 0,
	    "an OPL instrument and a 16-bit sample load as nothing");
	check(song.sampledata_size == SAMPLE_BYTES &&
	    (int8_t)song.sampledata[0] == -128 &&
	    (int8_t)song.sampledata[2] == 0 &&
	    (int8_t)song.sampledata[4] == 127,
	    "the samples are gathered as signed, whichever way the file "
	    "stores them");

	check(s3m_cell(m, 0, 0, 0)->note == 0x41 &&
	    s3m_cell(m, 0, 0, 0)->instrument == 1 &&
	    s3m_cell(m, 0, 0, 0)->volume == 32 &&
	    s3m_cell(m, 0, 0, 0)->command == 1 &&
	    s3m_cell(m, 0, 0, 0)->info == 6,
	    "a packed row's note, volume and command reach its cell");
	check(s3m_cell(m, 0, 0, 2)->note == 0x30 &&
	    s3m_cell(m, 0, 0, 1)->note == S3M_NOTE_NONE &&
	    s3m_cell(m, 0, 0, 1)->volume == S3M_VOLUME_NONE,
	    "... and a row says nothing about the channels it leaves out");
	check(s3m_cell(m, 0, 1, 1)->volume == 48 &&
	    s3m_cell(m, 0, 1, 1)->note == S3M_NOTE_NONE,
	    "a row can set a volume and no note");
	check(s3m_cell(m, 1, 0, 0)->note == S3M_NOTE_NONE,
	    "a pattern the file points nowhere for is empty");

	song_free(&song);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

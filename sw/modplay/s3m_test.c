/*
 * Host unit test for the ScreamTracker 3 parser and replayer (s3m.c,
 * st3.c).
 *
 *   make test            (from sw/modplay)
 *
 * The modules under test are assembled here rather than read from a
 * file, so each case states what it depends on.  The first is for the
 * parser: channels an OPL chip would play or the file has switched
 * off, orders that skip and end, panning both from the file's table
 * and from a channel's own side, unsigned samples, a loop running past
 * its sample, an instrument that is not sound at all, and rows packed
 * as the format packs them.  The second is for the replayer, a row to
 * each of the things Scream Tracker does its own way.
 */

#include <stdio.h>
#include <string.h>

#include "replay.h"

#define	ORDNUM		6
#define	INSNUM		3
#define	PATNUM		2

/* Where each part of the image goes; instruments follow the header's
 * lists, and a parapointer counts sixteenths.  Where the lists
 * themselves fall depends on how many orders and instruments a module
 * has, so each one says. */
#define	OFF_ORDERS	0x60
#define	OFF_INS_PARA	(OFF_ORDERS + ORDNUM)
#define	OFF_PAT_PARA	(OFF_INS_PARA + 2 * INSNUM)
#define	OFF_PAN		(OFF_PAT_PARA + 2 * PATNUM)
#define	INS_AT		0xC0	/* three headers of 0x50 */
#define	PAT_AT		0x200
#define	PAT1_AT		0x400
#define	SAMPLE_AT	0x600
#define	SAMPLE_BYTES	8

/* The replayer's module plays a longer sample, so that a start part
 * way into it lands inside. */
#define	PLAY_BYTES	1024
#define	PLAY_LOOP	512

static uint8_t image[SAMPLE_AT + PLAY_BYTES];
static size_t ins_para, pat_para;	/* the module's own lists */
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

	put16(ins_para + 2 * (n - 1), (uint32_t)(at / 16));
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

/* One channel's cell of a pattern, before it is packed. */
struct cell {
	int	row, ch, note, instrument, volume, command, info;
};

#define	NONE	(-1)

/*
 * Pack a pattern as the format packs it: a channel byte saying which
 * of note, volume and command follow, and a zero to end each row.  The
 * cells arrive in the order they are played.
 */
static void
pack(int pat, size_t at, const struct cell *cells, size_t n)
{
	size_t p = at + 2, i = 0;
	int row;

	for (row = 0; row < S3M_ROWS && i < n; row++) {
		for (; i < n && cells[i].row == row; i++) {
			const struct cell *c = &cells[i];
			int what = c->ch;

			if (c->note != NONE)
				what |= 32;
			if (c->volume != NONE)
				what |= 64;
			if (c->command != NONE)
				what |= 128;
			image[p++] = (uint8_t)what;
			if (c->note != NONE) {
				image[p++] = (uint8_t)c->note;
				image[p++] = (uint8_t)c->instrument;
			}
			if (c->volume != NONE)
				image[p++] = (uint8_t)c->volume;
			if (c->command != NONE) {
				image[p++] = (uint8_t)c->command;
				image[p++] = (uint8_t)c->info;
			}
		}
		image[p++] = 0;
	}
	put16(at, (uint32_t)(p - at));
	put16(pat_para + 2 * pat, (uint32_t)(at / 16));
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
	ins_para = OFF_INS_PARA;
	pat_para = OFF_PAT_PARA;
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

	/* Channel 2 is the OPL one and channel 3 is switched off, so
	 * what they play reaches no voice at all. */
	{
		static const struct cell cells[] = {
		    { 0, 0, 0x41, 1, 32, 1, 6 },	/* A: set speed */
		    { 0, 2, 0x30, 1, NONE, NONE, 0 },
		    { 0, 3, NONE, 0, 17, NONE, 0 },
		    { 0, 4, 0x30, 1, NONE, NONE, 0 },
		    { 1, 1, NONE, 0, 48, NONE, 0 },
		};

		pack(0, PAT_AT, cells, sizeof(cells) / sizeof(cells[0]));
	}
	memset(song, 0, sizeof(*song));
	if (!s3m_format.probe(image, sizeof(image)))
		return "the file is not taken for an S3M";
	song->format = &s3m_format;
	return s3m_format.parse(song, image, sizeof(image));
}

/* ── The replayer ────────────────────────────────────────────── */

/* What one tick played, for the checks to read back. */
struct snap {
	int	order, row, tick, bpm, speed;
	struct replay_voice voice;
};

static struct snap snaps[8192];
static int nsnaps;

/* A module of two orders, each row of the first putting one of Scream
 * Tracker's own ways to the replayer. */
static const char *
build_replay(struct song *song, bool fast_slides)
{
	static const struct cell rows[] = {
	    /* A note of its own, at the instrument's volume. */
	    { 0, 0, 0x40, 1, NONE, NONE, 0 },
	    /* A volume, then a slide of 2 a tick down from it. */
	    { 1, 0, NONE, 0, 60, 4, 0x02 },
	    /* A portamento down, which leaves 02 in the shared memory. */
	    { 2, 0, NONE, 0, NONE, 5, 0x02 },
	    /* A slide with no info of its own, which recalls it. */
	    { 3, 0, NONE, 0, NONE, 4, 0x00 },
	    /* A note starting part way into the sample. */
	    { 4, 0, 0x40, 1, NONE, 15, 0x01 },
	    { 5, 0, S3M_NOTE_CUT, 0, NONE, NONE, 0 },
	    /* A note after a cut, which needs an instrument to sound. */
	    { 6, 0, 0x40, 0, NONE, NONE, 0 },
	    { 7, 0, 0x40, 1, 60, NONE, 0 },
	    /* A retrigger every two ticks, eight quieter each time. */
	    { 8, 0, NONE, 0, NONE, 17, 0x42 },
	    { 9, 0, NONE, 0, NONE, 19, 0x85 },	/* S8: pan */
	    { 10, 0, NONE, 0, NONE, 20, 0x80 },	/* T: tempo 128 */
	    /* A tempo too low to run, and a break to row 10 in decimal. */
	    { 11, 1, NONE, 0, NONE, 20, 0x20 },
	    { 11, 0, NONE, 0, NONE, 3, 0x10 },
	};
	static const struct cell back[] = {
	    { 10, 0, NONE, 0, NONE, 2, 0x00 },	/* B: back to the first order */
	};
	static const uint8_t settings[] = { 0, 8 };
	size_t i;

	memset(image, 0, sizeof(image));
	ins_para = OFF_ORDERS + 2;	/* two orders */
	pat_para = ins_para + 2;	/* and one instrument */
	memcpy(image, "replay", 6);
	image[0x1C] = 0x1A;
	image[0x1D] = 16;
	put16(0x20, 2);			/* orders */
	put16(0x22, 1);			/* instruments */
	put16(0x24, 2);			/* patterns */
	put16(0x26, fast_slides ? 0x40 : 0);
	put16(0x28, 0x1320);
	put16(0x2A, 1);			/* signed samples */
	memcpy(&image[0x2C], "SCRM", 4);
	image[0x30] = 64;
	image[0x31] = 6;
	image[0x32] = 125;
	image[0x33] = 0x80 | 64;
	memset(&image[0x40], 255, 32);
	for (i = 0; i < sizeof(settings); i++)
		image[0x40 + i] = settings[i];
	image[0x60] = 0;
	image[0x61] = 1;

	instrument(1, 1, SAMPLE_AT, PLAY_BYTES, PLAY_LOOP, PLAY_BYTES, 0x01,
	    40);
	pack(0, PAT_AT, rows, sizeof(rows) / sizeof(rows[0]));
	pack(1, PAT1_AT, back, sizeof(back) / sizeof(back[0]));

	memset(song, 0, sizeof(*song));
	return song_parse(song, image, sizeof(image));
}

/* Play the song through, keeping what the first voice did on each
 * tick. */
static void
run(const struct song *song)
{
	struct replay_tick t;
	struct replay r;

	nsnaps = 0;
	replay_init(&r, song, 0, false);
	while (replay_next(&r, &t) && nsnaps < (int)(sizeof(snaps) /
	    sizeof(snaps[0]))) {
		struct snap *s = &snaps[nsnaps++];

		s->order = t.order;
		s->row = t.row;
		s->tick = t.tick;
		s->bpm = t.bpm;
		s->speed = t.speed;
		s->voice = t.voice[0];
	}
}

/* What the first voice played on one tick; never NULL, so that a case
 * reads as what it checks rather than as a search. */
static const struct snap *
at(int order, int row, int tick)
{
	static const struct snap none;
	int i;

	for (i = 0; i < nsnaps; i++)
		if (snaps[i].order == order && snaps[i].row == row &&
		    snaps[i].tick == tick)
			return &snaps[i];
	return &none;
}

static void
replay_cases(void)
{
	struct song song;
	const char *error;
	int i;

	if ((error = build_replay(&song, true)) != NULL) {
		printf("FAIL the replay module does not parse: %s\n", error);
		failures++;
		return;
	}
	run(&song);

	check(at(0, 0, 0)->voice.trigger && at(0, 0, 0)->voice.period == 1712 &&
	    at(0, 0, 0)->voice.volume == 40,
	    "a note plays at the period its octave and rate give it, at the "
	    "instrument's own volume");
	check(at(0, 0, 0)->voice.play.offset == 0 &&
	    at(0, 0, 0)->voice.play.length == PLAY_BYTES &&
	    at(0, 0, 0)->voice.loop.offset == PLAY_LOOP &&
	    at(0, 0, 0)->voice.loop.length == PLAY_BYTES - PLAY_LOOP,
	    "... playing through to the end of its loop, and then the loop");

	check(at(0, 1, 0)->voice.volume == 58 && at(0, 1, 5)->voice.volume == 48,
	    "a volume slide of the files ST3.00 wrote moves on the row's "
	    "first tick as well");
	check(at(0, 2, 5)->voice.period == 1712 + 5 * 8,
	    "a portamento down slides four period units a tick");
	check(at(0, 3, 0)->voice.volume == 46 && at(0, 3, 5)->voice.volume == 36,
	    "a slide with no info of its own recalls what another command "
	    "left in the shared memory");

	check(at(0, 4, 0)->voice.trigger &&
	    at(0, 4, 0)->voice.play.offset == 256,
	    "Oxx starts a note part way into its sample");
	check(at(0, 5, 0)->voice.trigger &&
	    at(0, 5, 0)->voice.play.length == 0 &&
	    at(0, 5, 0)->voice.loop.length == 0,
	    "a note cut leaves nothing to play, which stops the voice");
	check(!at(0, 6, 0)->voice.trigger,
	    "... and a note after it needs an instrument named with it");
	check(at(0, 7, 0)->voice.trigger &&
	    at(0, 7, 0)->voice.play.length == PLAY_BYTES,
	    "... which starts the sample again");

	check(!at(0, 8, 0)->voice.trigger && at(0, 8, 1)->voice.trigger &&
	    at(0, 8, 1)->voice.volume == 60 - 8 &&
	    at(0, 8, 3)->voice.trigger,
	    "Qxy retriggers every y ticks, quieter each time");
	check(at(0, 9, 0)->voice.pan == s3m_pan(5),
	    "S8x moves a voice across the stereo field");
	check(at(0, 10, 0)->bpm == 128 && at(0, 11, 0)->bpm == 128,
	    "Txx sets the tempo, and one too low to run is passed over");

	/* Cxx breaks to a row written in decimal, so 0x10 is row ten. */
	for (i = 0; i < nsnaps - 1; i++)
		if (snaps[i].order == 0 && snaps[i].row == 11 &&
		    snaps[i].tick == 5)
			break;
	check(i < nsnaps - 1 && snaps[i + 1].order == 1 &&
	    snaps[i + 1].row == 10,
	    "Cxx breaks to a row of the next order, in decimal");
	check(snaps[nsnaps - 1].order == 1 && snaps[nsnaps - 1].row == 10,
	    "Bxx jumps to an order by its place in the file, which brings "
	    "the song round");

	song_free(&song);

	if ((error = build_replay(&song, false)) != NULL) {
		printf("FAIL the replay module does not parse: %s\n", error);
		failures++;
		return;
	}
	run(&song);
	check(at(0, 1, 0)->voice.volume == 60 && at(0, 1, 5)->voice.volume == 50,
	    "without the ST3.00 flag, a volume slide waits for the row's "
	    "second tick");
	song_free(&song);
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
	check(m->pan[0] == 4 && m->pan[1] == 11 && m->pan[2] == 12 &&
	    s3m_pan(0) == -REPLAY_PAN && s3m_pan(15) == REPLAY_PAN &&
	    s3m_pan(4) == -30,
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

	check(song_unsupported(&song) != NULL &&
	    strstr(song_unsupported(&song), "AdLib instruments") != NULL &&
	    strstr(song_unsupported(&song), "16-bit samples") != NULL,
	    "the song says what it holds that modplay cannot play");

	song_free(&song);
	replay_cases();

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}

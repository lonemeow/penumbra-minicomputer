/*
 * ScreamTracker 3 module parsing.
 *
 * Layout: a 96-byte header, the orders, then a parapointer — an offset
 * in sixteenths — to each instrument and each pattern, and a byte of
 * panning per channel where the header says so.  The samples lie
 * wherever their instruments point, so they are gathered here into one
 * run of signed 8-bit memory, the shape both ways of playing want.
 *
 * A pattern packs its rows: a byte names a channel and says which of
 * note, volume and command follow, and a zero ends the row.  Rows are
 * stored unpacked, a cell for each channel the song plays, since the
 * replayer reads a row's channels in step.
 *
 * What the file cannot do is left as it lies: an order the song skips
 * keeps its place in the map a Bxx jump reads, and an instrument the
 * player cannot play loads as nothing rather than as noise.
 */

#include <stdlib.h>
#include <string.h>

#include "replay.h"

#define	OFF_TITLE	0x00
#define	OFF_ORDNUM	0x20
#define	OFF_INSNUM	0x22
#define	OFF_PATNUM	0x24
#define	OFF_FLAGS	0x26
#define	OFF_CWT		0x28
#define	OFF_FFI		0x2A
#define	OFF_SIGNATURE	0x2C
#define	OFF_GLOBAL_VOL	0x30
#define	OFF_SPEED	0x31
#define	OFF_TEMPO	0x32
#define	OFF_MASTER_VOL	0x33
#define	OFF_DEFAULT_PAN	0x35
#define	OFF_CHANNELS	0x40
#define	OFF_ORDERS	0x60

/* An instrument's header. */
#define	INS_TYPE	0x00
#define	INS_MEMSEG	0x0D	/* a parapointer, its high byte first */
#define	INS_LENGTH	0x10
#define	INS_LOOP_START	0x14
#define	INS_LOOP_END	0x18	/* the byte after the loop */
#define	INS_VOLUME	0x1C
#define	INS_PACK	0x1E
#define	INS_FLAGS	0x1F
#define	INS_C2SPD	0x20
#define	INS_NAME	0x30
#define	INS_BYTES	0x50

#define	INS_TYPE_SAMPLE	1	/* 0 is none, 2 and up are AdLib */
#define	INS_LOOPS	0x01
#define	INS_STEREO	0x02
#define	INS_16BIT	0x04

/* What the header says about the file's samples. */
#define	FFI_SIGNED	1	/* 2 means unsigned */

/* The file carries a byte of panning per channel. */
#define	DEFAULT_PAN_SET	252
#define	PAN_GIVEN	0x20	/* in such a byte: the rest of it counts */

/* A channel's setting: which side ST3's own mixing put it on, and
 * whether it plays at all. */
#define	CHANNEL_RIGHT	8	/* 0..7 are its left channels, 8..15 right */
#define	CHANNEL_ADLIB	16	/* 16..31 play on an OPL chip */
#define	CHANNEL_UNUSED	255
#define	CHANNEL_OFF	0x80	/* the channel is muted */

/* Where a pan of 0..15 leaves a voice, and where a file that sets none
 * leaves it: ST3's own defaults. */
#define	PAN_STEPS	15
#define	PAN_LEFT	3
#define	PAN_RIGHT	12
#define	PAN_MIDDLE	7

#define	ORDER_SKIP	254
#define	ORDER_END	255

static uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[1] << 8 | p[0];
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0];
}

int
s3m_pan(int place)
{
	int num = REPLAY_PAN * (2 * place - PAN_STEPS);

	return num >= 0 ? (num + PAN_STEPS / 2) / PAN_STEPS :
	    -((-num + PAN_STEPS / 2) / PAN_STEPS);
}

static bool
s3m_probe(const uint8_t *buf, size_t len)
{
	return len >= OFF_ORDERS &&
	    memcmp(buf + OFF_SIGNATURE, "SCRM", 4) == 0;
}

/* ── The header ──────────────────────────────────────────────── */

/*
 * The channels the song plays, in the order the file lists them, and
 * where each sits in the stereo field.  A channel an OPL chip would
 * play, or one the file has switched off, is not one of them: Scream
 * Tracker passes over such a channel entirely, effects and all.
 *
 * voice_of[] maps the channel a packed row names to the voice playing
 * it, or -1 where the song passes over it.
 */
static void
channels(struct song *song, const uint8_t *buf, size_t len)
{
	struct s3m *m = &song->s3m;
	const uint8_t *pan = NULL;
	bool stereo = (buf[OFF_MASTER_VOL] & 0x80) != 0;
	size_t pans = OFF_ORDERS + le16(buf + OFF_ORDNUM) +
	    2 * (size_t)le16(buf + OFF_INSNUM) +
	    2 * (size_t)le16(buf + OFF_PATNUM);
	int i;

	if (buf[OFF_DEFAULT_PAN] == DEFAULT_PAN_SET &&
	    len >= pans + S3M_CHANNELS)
		pan = buf + pans;

	for (i = 0; i < S3M_CHANNELS; i++) {
		int setting = buf[OFF_CHANNELS + i];
		int place;

		m->voice_of[i] = -1;
		if (setting >= CHANNEL_ADLIB) {
			if (setting < CHANNEL_UNUSED &&
			    !(setting & CHANNEL_OFF))
				m->unplayable |= S3M_HAS_ADLIB;
			continue;
		}
		place = !stereo ? PAN_MIDDLE :
		    setting < CHANNEL_RIGHT ? PAN_LEFT : PAN_RIGHT;
		if (pan != NULL && (pan[i] & PAN_GIVEN) != 0)
			place = pan[i] & 0xF;

		m->voice_of[i] = m->channels;
		m->pan[m->channels++] = place;
	}
	song->channels = m->channels;
}

/*
 * The orders the song plays.  A 254 is an order to skip, a 255 ends the
 * song, and an order naming a pattern the file does not hold is one
 * more to pass over.  A Bxx names an order by its place in the file, so
 * where each one ended up is kept.
 */
static void
orders(struct song *song, const uint8_t *buf)
{
	struct s3m *m = &song->s3m;
	int ordnum = (int)le16(buf + OFF_ORDNUM), i;

	for (i = 0; i < ordnum && i < S3M_ORDERS; i++) {
		int order = buf[OFF_ORDERS + i];

		/* The next order the song does play, for a jump that
		 * lands on one it does not. */
		m->order_of[i] = (uint8_t)song->length;
		if (order == ORDER_END)
			break;
		if (order == ORDER_SKIP || order >= song->npatterns)
			continue;
		song->order[song->length++] = (uint8_t)order;
	}
}

/* ── Instruments ─────────────────────────────────────────────── */

/*
 * Read the instrument headers, and lay their samples out end to end as
 * the player wants them.  Returns where each sample's data starts in
 * the file, since they are copied once the whole layout is known.
 */
static void
instruments(struct song *song, const uint8_t *buf, size_t len,
    uint32_t *data_at)
{
	struct s3m *m = &song->s3m;
	int insnum = (int)le16(buf + OFF_INSNUM);
	uint32_t off = 0;
	int s;

	for (s = 1; s <= insnum; s++) {
		size_t head = (size_t)le16(buf + OFF_ORDERS +
		    le16(buf + OFF_ORDNUM) + 2 * (s - 1)) * 16;
		struct s3m_sample *smp = &m->sample[s];
		const uint8_t *h = buf + head;
		uint32_t loop_start, loop_end;
		int flags;

		data_at[s] = 0;
		if (head == 0 || head + INS_BYTES > len)
			continue;
		song_text(smp->name, h + INS_NAME, 28);
		if (h[INS_TYPE] != INS_TYPE_SAMPLE) {
			if (h[INS_TYPE] != 0)
				m->unplayable |= S3M_HAS_ADLIB;
			continue;
		}

		smp->length = le32(h + INS_LENGTH);
		smp->volume = h[INS_VOLUME] > 64 ? 64 : h[INS_VOLUME];
		/* Scream Tracker reads the rate as sixteen bits. */
		smp->c2spd = le32(h + INS_C2SPD) & 0xFFFF;
		loop_start = le32(h + INS_LOOP_START);
		loop_end = le32(h + INS_LOOP_END);
		flags = h[INS_FLAGS];

		/* Anything but plain 8-bit sound loads as nothing, so
		 * that it is silent rather than noise. */
		if (h[INS_PACK] != 0)
			m->unplayable |= S3M_HAS_PACKED;
		else if ((flags & INS_16BIT) != 0)
			m->unplayable |= S3M_HAS_16BIT;
		else if ((flags & INS_STEREO) != 0)
			m->unplayable |= S3M_HAS_STEREO;
		else
			data_at[s] = (((uint32_t)h[INS_MEMSEG] << 16) |
			    le16(h + INS_MEMSEG + 1)) * 16;
		if (data_at[s] == 0 || data_at[s] >= len)
			smp->length = 0;
		else if (data_at[s] + smp->length > len)
			smp->length = (uint32_t)len - data_at[s];

		/* A loop is Scream Tracker's own once its ends are
		 * within the sample and the far one is past the near. */
		if ((flags & INS_LOOPS) != 0 && loop_end > loop_start &&
		    loop_start < smp->length) {
			if (loop_end > smp->length)
				loop_end = smp->length;
			smp->loop_start = loop_start;
			smp->loop_length = loop_end - loop_start;
		}

		smp->offset = off;
		off += smp->length;
	}
	m->nsamples = insnum;
	song->sampledata_size = off;
}

/* Gather the samples into one run, in the form the player plays:
 * signed, whatever the file stores. */
static const char *
sampledata(struct song *song, const uint8_t *buf, const uint32_t *data_at,
    bool is_signed)
{
	struct s3m *m = &song->s3m;
	uint8_t *data;
	uint32_t i;
	int s;

	if (song->sampledata_size == 0)
		return NULL;
	if ((data = malloc(song->sampledata_size)) == NULL)
		return "out of memory";
	song->sampledata = data;

	for (s = 1; s <= m->nsamples; s++) {
		const struct s3m_sample *smp = &m->sample[s];

		if (smp->length == 0)
			continue;
		memcpy(data + smp->offset, buf + data_at[s], smp->length);
		if (!is_signed)
			for (i = 0; i < smp->length; i++)
				data[smp->offset + i] ^= 0x80;
	}
	return NULL;
}

/* ── Patterns ────────────────────────────────────────────────── */

/*
 * Unpack one pattern.  A byte names a channel and which of note,
 * volume and command follow it; a zero byte ends a row.  A file that
 * stops in the middle leaves the rest of the pattern as it was, which
 * is empty.
 */
static void
pattern(struct s3m *m, int pat, const uint8_t *buf, size_t at, size_t len)
{
	size_t end;
	int row = 0;

	if (at == 0 || at + 2 > len)
		return;
	/* The length counts its own two bytes. */
	end = at + le16(buf + at);
	if (end > len)
		end = len;

	for (at += 2; row < S3M_ROWS && at < end; ) {
		struct s3m_cell *c = NULL;
		int what = buf[at++];

		if (what == 0) {
			row++;
			continue;
		}
		if (m->voice_of[what & 31] >= 0)
			c = &m->cell[(pat * S3M_ROWS + row) * m->channels +
			    m->voice_of[what & 31]];

		if ((what & 32) != 0) {
			if (at + 2 > end)
				break;
			if (c != NULL) {
				c->note = buf[at];
				c->instrument = buf[at + 1];
			}
			at += 2;
		}
		if ((what & 64) != 0) {
			if (at + 1 > end)
				break;
			if (c != NULL && buf[at] <= 64)
				c->volume = buf[at];
			at++;
		}
		if ((what & 128) != 0) {
			if (at + 2 > end)
				break;
			if (c != NULL) {
				c->command = buf[at];
				c->info = buf[at + 1];
			}
			at += 2;
		}
	}
}

static const char *
patterns(struct song *song, const uint8_t *buf, size_t len)
{
	struct s3m *m = &song->s3m;
	size_t para = OFF_ORDERS + le16(buf + OFF_ORDNUM) +
	    2 * (size_t)le16(buf + OFF_INSNUM);
	size_t cells = (size_t)song->npatterns * S3M_ROWS * m->channels;
	size_t i;
	int p;

	if (cells == 0)
		return NULL;
	if ((m->cell = malloc(cells * sizeof(*m->cell))) == NULL)
		return "out of memory";
	for (i = 0; i < cells; i++) {
		m->cell[i].note = S3M_NOTE_NONE;
		m->cell[i].instrument = 0;
		m->cell[i].volume = S3M_VOLUME_NONE;
		m->cell[i].command = 0;
		m->cell[i].info = 0;
	}

	for (p = 0; p < song->npatterns; p++)
		pattern(m, p, buf, (size_t)le16(buf + para + 2 * p) * 16, len);
	return NULL;
}

/* ── The file ────────────────────────────────────────────────── */

static const char *
s3m_parse(struct song *song, const uint8_t *buf, size_t len)
{
	struct s3m *m = &song->s3m;
	uint32_t data_at[S3M_SAMPLES + 1];
	const char *error;
	size_t header;
	int ordnum, insnum, patnum;

	if (len < OFF_ORDERS)
		return "too short for a module";
	ordnum = (int)le16(buf + OFF_ORDNUM);
	insnum = (int)le16(buf + OFF_INSNUM);
	patnum = (int)le16(buf + OFF_PATNUM);
	if (ordnum > S3M_ORDERS)
		return "more orders than the format holds";
	if (insnum > S3M_SAMPLES)
		return "more instruments than the format holds";
	if (patnum > S3M_ORDERS)
		return "more patterns than the format holds";
	header = OFF_ORDERS + (size_t)ordnum + 2 * (size_t)insnum +
	    2 * (size_t)patnum;
	if (len < header)
		return "header cut short";

	song_text(song->title, buf + OFF_TITLE, 28);
	memcpy(song->kind, "S3M", 4);
	song->clock = S3M_CLOCK;
	song->crossfeed = S3M_CROSSFEED;
	song->npatterns = patnum;

	m->flags = (int)le16(buf + OFF_FLAGS);
	m->cwt = (int)le16(buf + OFF_CWT);
	m->global_volume = buf[OFF_GLOBAL_VOL] > 64 ? 64 : buf[OFF_GLOBAL_VOL];
	m->master_volume = buf[OFF_MASTER_VOL] & 0x7F;
	/* Scream Tracker's own fallbacks for a header that asks for
	 * neither a speed nor a tempo it can run at. */
	m->speed = buf[OFF_SPEED] == 0 || buf[OFF_SPEED] == 255 ? 6 :
	    buf[OFF_SPEED];
	m->tempo = buf[OFF_TEMPO] < 33 ? 125 : buf[OFF_TEMPO];

	channels(song, buf, len);
	if (song->channels == 0)
		return "no channels to play";
	orders(song, buf);
	if (song->length == 0)
		return "no orders to play";

	instruments(song, buf, len, data_at);
	if ((error = sampledata(song, buf, data_at,
	    le16(buf + OFF_FFI) == FFI_SIGNED)) != NULL)
		return error;
	return patterns(song, buf, len);
}

static void
s3m_free(struct song *song)
{
	free(song->s3m.cell);
	song->s3m.cell = NULL;
	free((void *)song->sampledata);
	song->sampledata = NULL;
}

const struct song_format s3m_format = {
	.probe = s3m_probe,
	.parse = s3m_parse,
	.free = s3m_free,
	.unsupported = st3_unsupported,
	.start = st3_start,
	.play = st3_play,
};

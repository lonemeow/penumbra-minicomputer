/*
 * ProTracker module parsing.
 *
 * Layout: a 20-byte title, 31 sample headers of 30 bytes, the song
 * length and a restart byte, 128 orders, a four-byte signature, then
 * the patterns — as many as the highest pattern number in all 128
 * orders, 1 KB each — and finally the samples back to back.  Every
 * length in a sample header is in 16-bit words.
 */

#include <stdlib.h>
#include <string.h>

#include "song.h"

#define	OFF_SAMPLES	20
#define	SAMPLE_HEADER	30
#define	OFF_LENGTH	950
#define	OFF_ORDERS	952
#define	OFF_SIGNATURE	1080
#define	OFF_PATTERNS	1084
#define	PATTERN_BYTES	(MOD_ROWS * MOD_CHANNELS * 4)

/* A PAL Amiga's Paula plays a sample at this rate divided by the
 * period. */
#define	PAULA_HZ	3546895

/*
 * The signatures of the four-channel, 31-sample layout, whichever
 * tracker wrote it: M.K. is ProTracker's, M!K! its mark for more than
 * 64 patterns, 4CHN FastTracker's, and FLT4 StarTrekker's name for the
 * same format rather than a variant of it.
 *
 * StarTrekker's other signatures are left out on purpose.  EXO4 and
 * EXO8 mark instruments it synthesises, which modplay does not, and
 * FLT8 pairs its patterns into eight channels, so read as four it would
 * play half of every pattern as a pattern of its own.
 */
static const char *const signatures[] = { "M.K.", "M!K!", "FLT4", "4CHN" };

static uint32_t
be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | p[1];
}

/* Copy a fixed-width text field, stopping at its first NUL and keeping
 * the result printable. */
static void
text(char *dst, const uint8_t *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '.';
	dst[i] = '\0';
}

static bool
mod_probe(const uint8_t *buf, size_t len)
{
	size_t i;

	if (len < OFF_PATTERNS)
		return false;
	for (i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++)
		if (memcmp(buf + OFF_SIGNATURE, signatures[i], 4) == 0)
			return true;
	return false;
}

static const char *
mod_parse(struct song *song, const uint8_t *buf, size_t len)
{
	struct mod *m = &song->mod;
	size_t data, avail;
	uint32_t off;
	int s, i, n;

	if (len < OFF_PATTERNS)
		return "too short for a module";

	memcpy(song->kind, buf + OFF_SIGNATURE, 4);
	text(song->title, buf, 20);
	song->channels = MOD_CHANNELS;
	song->clock = PAULA_HZ;
	song->crossfeed = MOD_CROSSFEED;

	for (s = 1; s <= MOD_SAMPLES; s++) {
		const uint8_t *h = buf + OFF_SAMPLES + (s - 1) * SAMPLE_HEADER;
		struct mod_sample *smp = &m->sample[s];

		text(smp->name, h, 22);
		smp->length = be16(h + 22) * 2;
		smp->finetune = ((h[24] & 0xF) ^ 8) - 8;
		smp->volume = h[25] > 64 ? 64 : h[25];
		smp->loop_start = be16(h + 26) * 2;
		smp->loop_length = be16(h + 28) * 2;
	}

	song->length = buf[OFF_LENGTH];
	if (song->length < 1 || song->length > MOD_ORDERS)
		return "song length out of range";
	memcpy(song->order, buf + OFF_ORDERS, MOD_ORDERS);
	for (i = 0; i < MOD_ORDERS; i++)
		if (song->order[i] + 1 > song->npatterns)
			song->npatterns = song->order[i] + 1;

	data = OFF_PATTERNS + (size_t)song->npatterns * PATTERN_BYTES;
	if (len < data)
		return "patterns cut short";

	m->pattern = calloc((size_t)song->npatterns * MOD_ROWS * MOD_CHANNELS,
	    sizeof(*m->pattern));
	if (m->pattern == NULL)
		return "out of memory";
	for (n = 0; n < song->npatterns * MOD_ROWS * MOD_CHANNELS; n++) {
		const uint8_t *c = buf + OFF_PATTERNS + n * 4;
		struct mod_note *note = &m->pattern[n];

		note->period = (c[0] & 0x0F) << 8 | c[1];
		note->sample = (c[0] & 0xF0) | c[2] >> 4;
		note->effect = c[2] & 0x0F;
		note->param = c[3];
		if (note->sample > MOD_SAMPLES)
			note->sample = 0;
	}

	/*
	 * The samples.  A file that ends early loses the tail of whatever
	 * it cuts into; a loop that runs past its sample's end is cut to
	 * it, and one of a single word is ProTracker's "no loop".
	 */
	avail = len - data;
	off = 0;
	for (s = 1; s <= MOD_SAMPLES; s++) {
		struct mod_sample *smp = &m->sample[s];

		smp->offset = off;
		if (off + smp->length > avail)
			smp->length = off < avail ? (uint32_t)(avail - off) : 0;
		off += smp->length;

		if (smp->loop_length <= 2 || smp->loop_start >= smp->length)
			smp->loop_length = 0;
		else if (smp->loop_start + smp->loop_length > smp->length)
			smp->loop_length = smp->length - smp->loop_start;
		if (smp->loop_length == 0)
			smp->loop_start = 0;
	}
	song->sampledata = buf + data;
	song->sampledata_size = off;
	return NULL;
}

static void
mod_free(struct song *song)
{
	free(song->mod.pattern);
	song->mod.pattern = NULL;
}

const struct song_format mod_format = {
	.probe = mod_probe,
	.parse = mod_parse,
	.free = mod_free,
	.unsupported = pt_unsupported,
	.start = pt_start,
	.play = pt_play,
};

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

#include "mod.h"

#define	OFF_SAMPLES	20
#define	SAMPLE_HEADER	30
#define	OFF_LENGTH	950
#define	OFF_ORDERS	952
#define	OFF_SIGNATURE	1080
#define	OFF_PATTERNS	1084
#define	PATTERN_BYTES	(MOD_ROWS * MOD_CHANNELS * 4)

/* The signatures of four-channel, 31-sample modules. */
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

const char *
mod_parse(struct mod *m, const uint8_t *buf, size_t len)
{
	size_t data, avail;
	uint32_t off;
	int s, i, n;

	memset(m, 0, sizeof(*m));
	if (len < OFF_PATTERNS)
		return "too short for a module";

	memcpy(m->signature, buf + OFF_SIGNATURE, 4);
	for (i = 0; i < (int)(sizeof(signatures) / sizeof(signatures[0])); i++)
		if (memcmp(m->signature, signatures[i], 4) == 0)
			break;
	if (i == (int)(sizeof(signatures) / sizeof(signatures[0])))
		return "not a four-channel, 31-sample module";

	text(m->title, buf, 20);

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

	m->length = buf[OFF_LENGTH];
	if (m->length < 1 || m->length > MOD_ORDERS)
		return "song length out of range";
	memcpy(m->order, buf + OFF_ORDERS, MOD_ORDERS);
	for (i = 0; i < MOD_ORDERS; i++)
		if (m->order[i] + 1 > m->npatterns)
			m->npatterns = m->order[i] + 1;

	data = OFF_PATTERNS + (size_t)m->npatterns * PATTERN_BYTES;
	if (len < data)
		return "patterns cut short";

	m->pattern = calloc((size_t)m->npatterns * MOD_ROWS * MOD_CHANNELS,
	    sizeof(*m->pattern));
	if (m->pattern == NULL)
		return "out of memory";
	for (n = 0; n < m->npatterns * MOD_ROWS * MOD_CHANNELS; n++) {
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
	m->sampledata = buf + data;
	m->sampledata_size = off;
	return NULL;
}

void
mod_free(struct mod *m)
{
	free(m->pattern);
	m->pattern = NULL;
}

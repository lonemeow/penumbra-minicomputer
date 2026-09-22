/*
 * Songs: finding the format a file is in.
 */

#include <string.h>

#include "song.h"

static const struct song_format *const formats[] = {
	&mod_format,
};

const char *
song_parse(struct song *s, const uint8_t *buf, size_t len)
{
	size_t i;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
		if (formats[i]->probe(buf, len)) {
			s->format = formats[i];
			return formats[i]->parse(s, buf, len);
		}
	return "unsupported file format";
}

void
song_free(struct song *s)
{
	if (s->format != NULL)
		s->format->free(s);
}

const char *
song_unsupported(const struct song *s)
{
	return s->format->unsupported(s);
}

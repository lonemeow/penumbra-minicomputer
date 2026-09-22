/*
 * modplay-dump: walk a module's song on the build host without playing
 * it, through the same parser and replayer modplay uses.
 *
 * Prints the module's header, the effects the replayer ignores, and
 * how long the song runs; with -r, also each row as it is played, one
 * column per channel: period, volume, and * where a note starts.
 *
 * With -w, it also mixes the song as modplay mixes one in software,
 * into a WAV file, to be heard without the machine; -R sets the rate.
 *
 * usage: modplay-dump [-r] [-R rate] [-w file.wav] file ...
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mixer.h"
#include "replay.h"

#define	WAV_HEADER	44
#define	WAV_FRAMES	1024

static uint8_t *
slurp(const char *path, size_t *lenp)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long len;

	if (f == NULL)
		err(1, "%s", path);
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0)
		err(1, "%s", path);
	buf = malloc(len > 0 ? (size_t)len : 1);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len)
		err(1, "%s", path);
	fclose(f);
	*lenp = (size_t)len;
	return buf;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* A 16-bit stereo PCM WAV header for so many bytes of samples. */
static void
wav_header(uint8_t *h, uint32_t rate, uint32_t bytes)
{
	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	put32(h + 20, 1 | 2 << 16);		/* PCM, two channels */
	put32(h + 24, rate);
	put32(h + 28, rate * 4);		/* bytes a second */
	put32(h + 32, 4 | 16 << 16);		/* bytes a frame, bits */
	memcpy(h + 36, "data", 4);
	put32(h + 40, bytes);
}

/* Mix the song as modplay would through the software mixer, at its
 * own crossfeed, into a WAV file. */
static void
render(const struct song *s, const char *wav, uint32_t rate)
{
	static struct mixer mx;
	static int16_t out[WAV_FRAMES * 2];
	static uint8_t bytes[WAV_FRAMES * 4];
	uint8_t header[WAV_HEADER];
	struct replay r;
	struct replay_tick t;
	uint32_t n, i, total = 0;
	FILE *f;

	if ((f = fopen(wav, "wb")) == NULL)
		err(1, "%s", wav);
	wav_header(header, rate, 0);
	fwrite(header, 1, WAV_HEADER, f);

	mixer_init(&mx, s, rate, s->crossfeed);
	replay_init(&r, s, 0, false);
	while (replay_next(&r, &t)) {
		mixer_tick(&mx, &t);
		while ((n = mixer_mix(&mx, out, WAV_FRAMES)) > 0) {
			for (i = 0; i < 2 * n; i++) {
				bytes[2 * i] = (uint8_t)out[i];
				bytes[2 * i + 1] = (uint8_t)(out[i] >> 8);
			}
			fwrite(bytes, 4, n, f);
			total += 4 * n;
		}
	}

	wav_header(header, rate, total);
	if (fseek(f, 0, SEEK_SET) != 0 ||
	    fwrite(header, 1, WAV_HEADER, f) != WAV_HEADER || fclose(f) != 0)
		err(1, "%s", wav);
	printf("  mixed into %s at %u Hz\n", wav, rate);
}

static void
dump(const char *path, bool rows, const char *wav, uint32_t rate)
{
	struct song song;
	struct replay r;
	struct replay_tick t;
	const char *error, *unsupported;
	uint64_t us = 0;
	uint8_t *buf;
	size_t len;
	int ch;

	buf = slurp(path, &len);
	if ((error = song_parse(&song, buf, len)) != NULL)
		errx(1, "%s: %s", path, error);

	printf("%s: \"%s\" %s, %d orders, %d patterns, %u bytes of samples\n",
	    path, song.title, song.kind, song.length, song.npatterns,
	    song.sampledata_size);
	if ((unsupported = song_unsupported(&song)) != NULL)
		printf("  ignored: %s\n", unsupported);

	replay_init(&r, &song, 0, false);
	while (replay_next(&r, &t)) {
		if (rows && t.tick == 0) {
			printf("%02d %02d %02d  %2d %3d |", t.order, t.pattern,
			    t.row, t.speed, t.bpm);
			for (ch = 0; ch < song.channels; ch++) {
				const struct replay_voice *v = &t.voice[ch];

				printf(" %4d %2d%c", v->period, v->volume,
				    v->trigger ? '*' : ' ');
			}
			putchar('\n');
		}
		us += REPLAY_TICK_US((uint64_t)t.bpm);
	}
	printf("  plays %u:%02u.%03u\n", (unsigned)(us / 60000000),
	    (unsigned)(us / 1000000 % 60), (unsigned)(us / 1000 % 1000));
	if (wav != NULL)
		render(&song, wav, rate);

	song_free(&song);
	free(buf);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: modplay-dump [-r] [-R rate] [-w file.wav] file ...\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *wav = NULL;
	unsigned long rate = 24000;
	bool rows = false;
	char *end;
	int ch;

	while ((ch = getopt(argc, argv, "rR:w:")) != -1) {
		switch (ch) {
		case 'r':
			rows = true;
			break;
		case 'R':
			rate = strtoul(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    rate < 1000 || rate > 192000)
				usage();
			break;
		case 'w':
			wav = optarg;
			break;
		default:
			usage();
		}
	}
	/* One file to a WAV. */
	if (optind == argc || (wav != NULL && argc - optind != 1))
		usage();
	for (; optind < argc; optind++)
		dump(argv[optind], rows, wav, (uint32_t)rate);
	return 0;
}

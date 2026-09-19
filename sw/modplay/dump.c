/*
 * modplay-dump: walk a module's song on the build host without playing
 * it, through the same parser and replayer modplay uses.
 *
 * Prints the module's header, the effects the replayer ignores, and
 * how long the song runs; with -r, also each row as it is played, one
 * column per channel: period, volume, and * where a note starts.
 *
 * usage: modplay-dump [-r] file ...
 */

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mod.h"
#include "replay.h"

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
dump(const char *path, bool rows)
{
	struct mod m;
	struct replay r;
	struct replay_tick t;
	const char *error, *unsupported;
	uint64_t us = 0;
	uint8_t *buf;
	size_t len;
	int ch;

	buf = slurp(path, &len);
	if ((error = mod_parse(&m, buf, len)) != NULL)
		errx(1, "%s: %s", path, error);

	printf("%s: \"%s\" %s, %d orders, %d patterns, %u bytes of samples\n",
	    path, m.title, m.signature, m.length, m.npatterns,
	    m.sampledata_size);
	if ((unsupported = replay_unsupported(&m)) != NULL)
		printf("  ignored: %s\n", unsupported);

	replay_init(&r, &m, 0, false);
	while (replay_next(&r, &t)) {
		if (rows && t.tick == 0) {
			printf("%02d %02d %02d  %2d %3d |", t.order, t.pattern,
			    t.row, t.speed, t.bpm);
			for (ch = 0; ch < MOD_CHANNELS; ch++) {
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

	mod_free(&m);
	free(buf);
}

int
main(int argc, char **argv)
{
	bool rows = false;
	int ch;

	while ((ch = getopt(argc, argv, "r")) != -1) {
		switch (ch) {
		case 'r':
			rows = true;
			break;
		default:
			fprintf(stderr, "usage: modplay-dump [-r] file ...\n");
			return 2;
		}
	}
	if (optind == argc) {
		fprintf(stderr, "usage: modplay-dump [-r] file ...\n");
		return 2;
	}
	for (; optind < argc; optind++)
		dump(argv[optind], rows);
	return 0;
}

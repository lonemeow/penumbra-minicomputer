/*
 * modplay: play ProTracker modules through /dev/pmusic.
 *
 * The replayer turns a song into ticks; this file turns each tick into
 * /dev/pmusic events — a period into an increment, a volume and the
 * channel's place in the stereo field into a gain for each side — and
 * submits them a little ahead of the kernel's clock, which then keeps
 * the time.  The samples go into the device's sample memory once, as
 * the file stores them, so a sample's offset there is its offset in
 * the file's sample data.
 *
 * usage: modplay [-Lil] [-c crossfeed] [-d display] [-o tty] [-p order]
 *                 file ...
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <machine/penaudio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "display.h"
#include "mod.h"
#include "replay.h"

#define	DEVICE		"/dev/pmusic"

/* A PAL Amiga's Paula plays a sample at this rate divided by the
 * period. */
#define	PAULA_HZ	3546895

#define	LOOKAHEAD	50	/* ticks generated ahead of the clock */
#define	LOWAT		25	/* wake when fewer ticks than this remain */
#define	PEND_MAX	512
#define	EVENTS_PER_TICK	(MOD_CHANNELS + 1)	/* a voice each, and tempo */
#define	TIMELINE	256	/* ticks kept for the display; > LOOKAHEAD */
#define	POLL_MS		40	/* how often the display looks */

/* Most modules sound harsh panned hard; this leaves each channel
 * clearly on its side. */
#define	CROSSFEED	75	/* percent */

struct player {
	int		fd;
	const struct mod *mod;
	struct replay	replay;

	uint32_t	gen;		/* next tick to generate */
	bool		done;		/* the final stop is generated */
	uint32_t	last;		/*  ... on this tick */
	struct penaudio_event pend[PEND_MAX];
	uint32_t	npend;		/* generated, not yet accepted */

	/* What each voice was last told, so only changes are sent. */
	struct {
		int		sample;		/* 0 = silent */
		uint32_t	inc;
		uint16_t	vol_l, vol_r;
	} voice[MOD_CHANNELS];
	int		bpm;

	/*
	 * The ticks generated but not yet all heard, indexed by tick: the
	 * display shows the one the clock is on, which is well behind the
	 * one being generated.
	 */
	struct display_tick timeline[TIMELINE];
	uint64_t	us;		/* song time generated */
};

/* What the command line asked for, beyond which files to play. */
struct options {
	bool		ignore;		/* play despite effects modplay skips */
	bool		loop;
	int		start;		/* the order to begin at */
	int		outfd;		/* the terminal the display draws on */
};

static struct penaudio_info info;
static volatile sig_atomic_t quit;

/* INC at a period of 1, for the device's rate: 16.16, like INC. */
static uint32_t paula_inc;

static void
on_signal(int sig)
{
	quit = 1;
}

/* ── Voices ──────────────────────────────────────────────────── */

/* The increment that plays a sample at a period, rounded. */
static uint32_t
period_inc(int period)
{
	uint32_t inc = (paula_inc + period / 2) / period;

	if (inc < PENAUDIO_INC_MIN)
		return PENAUDIO_INC_MIN;
	if (inc > PENAUDIO_INC_MAX)
		return PENAUDIO_INC_MAX;
	return inc;
}

/*
 * Each side's gain for a channel, by its volume (0..64).  Paula puts
 * channels 0 and 3 on the left and 1 and 2 on the right; crossfeed
 * gives the opposite side a share of what the channel's own side gets.
 *
 * pmusic gains run from 0x0000, silent, to 0xFFFF, unity, and the
 * device saturates a side's sum once, after mixing.  Every voice
 * arrives at full 16-bit scale, since each 8-bit sample is shifted up,
 * so the gains alone decide whether a loud passage clips: a channel's
 * two sides always add up to half scale at volume 64, so four channels
 * at full volume sum to exactly full scale on either side, whatever
 * the crossfeed.
 */
static uint16_t gain_near[65], gain_far[65];

static void
gains_init(int crossfeed)
{
	int volume;

	for (volume = 0; volume <= 64; volume++) {
		uint32_t gain = volume * 0xFFFF / 64 / 2;

		gain_near[volume] = gain * 100 / (100 + crossfeed);
		gain_far[volume] = gain * crossfeed / (100 + crossfeed);
	}
}

static void
voice_gain(int ch, int volume, uint16_t *left, uint16_t *right)
{
	switch (ch) {
	case 0:
	case 3:
		*left = gain_near[volume];
		*right = gain_far[volume];
		break;
	case 1:
	case 2:
		*left = gain_far[volume];
		*right = gain_near[volume];
		break;
	}
}

/*
 * Where a sample starts: ProTracker plays a looping sample through to
 * the end of its loop first, or to the end of the sample when the loop
 * starts at 0, and then repeats the loop.  Returns false when there is
 * nothing to play — an empty sample, or an offset past the end of one
 * that does not loop.
 */
static bool
sample_start(const struct mod_sample *smp, uint32_t offset,
    struct penaudio_event *e)
{
	uint32_t first = smp->length;

	if (smp->loop_length != 0 && smp->loop_start != 0)
		first = smp->loop_start + smp->loop_length;

	e->pe_format = PENAUDIO_FMT_S8;
	e->pe_loop.pr_offset = smp->offset + smp->loop_start;
	e->pe_loop.pr_length = smp->loop_length;
	if (offset < first) {
		e->pe_play.pr_offset = smp->offset + offset;
		e->pe_play.pr_length = first - offset;
	}
	return e->pe_play.pr_length != 0 || e->pe_loop.pr_length != 0;
}

/* One voice's event for a tick: whatever changed since the last. */
static void
voice_event(struct player *p, int ch, const struct replay_voice *v,
    struct penaudio_event *e)
{
	const struct mod_sample *smp = &p->mod->sample[v->sample];
	uint16_t vol_l, vol_r;
	uint32_t inc;
	bool started = false;

	if (v->sample == 0 || v->period == 0)
		return;

	if (v->trigger) {
		if (sample_start(smp, v->offset, e)) {
			e->pe_set |= PENAUDIO_SET_PLAY;
			p->voice[ch].sample = v->sample;
			started = true;
		} else {
			e->pe_set |= PENAUDIO_SET_STOP;
			p->voice[ch].sample = 0;
			return;
		}
	} else if (v->swap && p->voice[ch].sample != 0 &&
	    v->sample != p->voice[ch].sample) {
		e->pe_set |= PENAUDIO_SET_LOOP;
		e->pe_format = PENAUDIO_FMT_S8;
		e->pe_loop.pr_offset = smp->offset + smp->loop_start;
		e->pe_loop.pr_length = smp->loop_length;
		p->voice[ch].sample = v->sample;
	}

	inc = period_inc(v->period);
	if (started || inc != p->voice[ch].inc) {
		e->pe_set |= PENAUDIO_SET_INC;
		e->pe_inc = inc;
		p->voice[ch].inc = inc;
	}
	voice_gain(ch, v->volume, &vol_l, &vol_r);
	if (started || vol_l != p->voice[ch].vol_l ||
	    vol_r != p->voice[ch].vol_r) {
		e->pe_set |= PENAUDIO_SET_VOL;
		e->pe_vol_l = vol_l;
		e->pe_vol_r = vol_r;
		p->voice[ch].vol_l = vol_l;
		p->voice[ch].vol_r = vol_r;
	}
}

/* ── Feeding ─────────────────────────────────────────────────── */

/* The next tick's events; after the song's last tick, a tick that
 * silences every voice. */
static uint32_t
gen_tick(struct player *p, struct penaudio_event *ev)
{
	struct replay_tick t;
	uint32_t tick = p->gen++, n = 0;
	int ch;

	if (!replay_next(&p->replay, &t)) {
		for (ch = 0; ch < MOD_CHANNELS; ch++) {
			memset(&ev[n], 0, sizeof(ev[n]));
			ev[n].pe_tick = tick;
			ev[n].pe_voice = ch;
			ev[n++].pe_set = PENAUDIO_SET_STOP;
		}
		p->done = true;
		p->last = tick;
		return n;
	}

	if (t.bpm != p->bpm) {
		memset(&ev[n], 0, sizeof(ev[n]));
		ev[n].pe_tick = tick;
		ev[n].pe_voice = PENAUDIO_VOICE_NONE;
		ev[n].pe_set = PENAUDIO_SET_PERIOD;
		ev[n++].pe_period_us = REPLAY_TICK_US(t.bpm);
		p->bpm = t.bpm;
	}
	for (ch = 0; ch < MOD_CHANNELS; ch++) {
		memset(&ev[n], 0, sizeof(ev[n]));
		ev[n].pe_tick = tick;
		ev[n].pe_voice = ch;
		voice_event(p, ch, &t.voice[ch], &ev[n]);
		if (ev[n].pe_set != 0)
			n++;
	}

	p->timeline[tick % TIMELINE].tick = t;
	p->timeline[tick % TIMELINE].us = p->us;
	p->us += REPLAY_TICK_US(t.bpm);
	return n;
}

/*
 * Generate up to LOOKAHEAD ticks past the clock and submit them.
 * Returns false once everything, the final stop included, is taken.
 */
static bool
feed(struct player *p)
{
	struct penaudio_status st;
	struct penaudio_submit ps;
	uint32_t target;

	if (ioctl(p->fd, PENAUDIO_GETSTATUS, &st) == -1)
		err(1, "GETSTATUS");

	target = st.pst_tick + LOOKAHEAD;
	while (!p->done && p->gen <= target &&
	    p->npend + EVENTS_PER_TICK <= PEND_MAX)
		p->npend += gen_tick(p, &p->pend[p->npend]);

	memset(&ps, 0, sizeof(ps));
	ps.ps_events = p->pend;
	ps.ps_eventsize = sizeof(p->pend[0]);
	ps.ps_nevents = p->npend;
	ps.ps_through = p->gen - 1;
	if (ioctl(p->fd, PENAUDIO_SUBMIT, &ps) == -1)
		err(1, "SUBMIT");
	p->npend -= ps.ps_nevents;
	memmove(p->pend, &p->pend[ps.ps_nevents],
	    p->npend * sizeof(p->pend[0]));

	return !p->done || p->npend > 0;
}

/* ── Files ───────────────────────────────────────────────────── */

static uint8_t *
slurp(const char *path, size_t *lenp)
{
	uint8_t *buf = NULL;
	size_t len = 0, cap = 0;
	ssize_t n;
	int fd;

	if ((fd = open(path, O_RDONLY)) == -1) {
		warn("%s", path);
		return NULL;
	}
	for (;;) {
		if (len == cap) {
			cap = cap != 0 ? cap * 2 : 256 * 1024;
			if ((buf = realloc(buf, cap)) == NULL)
				err(1, "%s", path);
		}
		if ((n = read(fd, buf + len, cap - len)) <= 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (n == -1) {
		warn("%s", path);
		free(buf);
		return NULL;
	}
	*lenp = len;
	return buf;
}

/*
 * Print where a module's cues start, as the -p values that play them.
 * Needs no device: a module holding a game's music keeps its tunes
 * behind its jingles, and the order each starts at is not in the file
 * to read off.
 */
static bool
list(const char *path)
{
	static struct replay_cue cue[MOD_ORDERS];
	struct mod m;
	const char *error;
	uint8_t *buf;
	size_t len;
	int n, i;

	if ((buf = slurp(path, &len)) == NULL)
		return false;
	if ((error = mod_parse(&m, buf, len)) != NULL) {
		warnx("%s: %s", path, error);
		free(buf);
		return false;
	}

	printf("%s: \"%s\" %s, %d orders\n", path, m.title, m.signature,
	    m.length);
	n = replay_scan(&m, cue, MOD_ORDERS);
	for (i = 0; i < n; i++) {
		unsigned s = (unsigned)(cue[i].us / 1000000);

		printf("  -p %-3d %2d %-6s  %u:%02u.%u  ", cue[i].start,
		    cue[i].orders, cue[i].orders == 1 ? "order" : "orders",
		    s / 60, s % 60, (unsigned)(cue[i].us / 100000 % 10));
		if (cue[i].loop >= 0)
			printf("loops to order %d\n", cue[i].loop);
		else
			printf("ends\n");
	}

	mod_free(&m);
	free(buf);
	return true;
}

/* Returns false if the file could not be played. */
static bool
play(int fd, const char *path, const struct options *opt)
{
	static struct player p;
	struct penaudio_clock pc;
	struct pollfd pfd;
	struct penaudio_status st;
	int64_t shown = -1;		/* the last tick the display has seen */
	struct mod m;
	const char *error, *unsupported;
	uint8_t *buf;
	size_t len;
	bool more;

	if ((buf = slurp(path, &len)) == NULL)
		return false;
	if ((error = mod_parse(&m, buf, len)) != NULL) {
		warnx("%s: %s", path, error);
		free(buf);
		return false;
	}

	if (opt->start >= m.length) {
		warnx("%s: has %d orders, so -p %d starts past its end", path,
		    m.length, opt->start);
		goto fail;
	}

	unsupported = replay_unsupported(&m);
	if (unsupported != NULL && !opt->ignore) {
		warnx("%s: uses %s, which modplay does not play; "
		    "-i plays it without", path, unsupported);
		goto fail;
	}
	if (m.sampledata_size > info.pi_maxmem) {
		warnx("%s: needs %u bytes of sample memory, and "
		    "machdep.paudio.maxmem allows %u", path,
		    m.sampledata_size, info.pi_maxmem);
		goto fail;
	}

	if (!display_start(&m, path, opt->outfd))
		goto fail;
	if (unsupported != NULL)
		printf("  playing without %s\n", unsupported);

	if (ioctl(fd, PENAUDIO_RESET) == -1)
		err(1, "RESET");
	if (ioctl(fd, PENAUDIO_SETMEM, &m.sampledata_size) == -1)
		err(1, "SETMEM");
	if (m.sampledata_size != 0 &&
	    pwrite(fd, m.sampledata, m.sampledata_size, 0) !=
	    (ssize_t)m.sampledata_size)
		err(1, "loading samples");

	memset(&p, 0, sizeof(p));
	p.fd = fd;
	p.mod = &m;
	p.bpm = 125;		/* ProTracker's; the song may change it */
	replay_init(&p.replay, &m, opt->start, opt->loop);

	/*
	 * The clock starts at the first tick's tempo, which is known once
	 * that tick is generated — unless the song stopped before its
	 * first tick, leaving nothing to time.
	 */
	more = feed(&p);
	if (!(p.done && p.last == 0)) {
		pc.pc_period_us = REPLAY_TICK_US(p.timeline[0].tick.bpm);
		pc.pc_lowat = LOWAT;
		if (ioctl(fd, PENAUDIO_START, &pc) == -1)
			err(1, "START");
	}

	pfd.fd = fd;
	pfd.events = POLLOUT;
	while (!quit) {
		/* Once everything is submitted, only the display wakes. */
		if (!more)
			pfd.events = 0;
		if (poll(&pfd, 1, POLL_MS) == -1 && errno != EINTR)
			err(1, "poll");
		if (quit)
			break;
		if (more && (pfd.revents & POLLOUT))
			more = feed(&p);

		if (ioctl(fd, PENAUDIO_GETSTATUS, &st) == -1)
			err(1, "GETSTATUS");
		if (!more && st.pst_tick >= p.last)
			break;

		/*
		 * Every tick reaches the display, not only the one the
		 * clock stands on: a note starts for a single tick, and
		 * the wait above spans several of them.
		 */
		if ((int64_t)st.pst_tick > shown) {
			int64_t tick = shown + 1;
			int64_t oldest = (int64_t)st.pst_tick - (TIMELINE - 1);

			if (tick < oldest)	/* the ring has come round */
				tick = oldest;
			if (tick < 0)
				tick = 0;
			for (; tick <= (int64_t)st.pst_tick; tick++)
				display_update(&p.timeline[tick % TIMELINE]);
			shown = st.pst_tick;
		}
	}
	display_end();

	/* Silence at once, rather than letting queued ticks play out. */
	if (ioctl(fd, PENAUDIO_RESET) == -1)
		err(1, "RESET");
	mod_free(&m);
	free(buf);
	return true;

fail:
	mod_free(&m);
	free(buf);
	return false;
}

static void
usage(void)
{
	int i;

	fprintf(stderr,
	    "usage: modplay [-Lil] [-c crossfeed] [-d display] [-o tty] "
	    "[-p order] file ...\n"
	    "  -c  percent of each channel's own side that the other side "
	    "gets:\n"
	    "      0 pans hard, as the Amiga does, 100 is mono; default %d\n"
	    "  -d  how to show the song while it plays:\n", CROSSFEED);
	for (i = 0; display_backends[i] != NULL; i++)
		fprintf(stderr, "        %-8s%s%s\n",
		    display_backends[i]->name, display_backends[i]->summary,
		    i == 0 ? " (default)" : "");
	fprintf(stderr,
	    "  -i  play a module even if it uses effects modplay ignores\n"
	    "  -L  list where each file's cues start instead of playing\n"
	    "  -l  loop each song rather than stopping at its end\n"
	    "  -o  draw the display on this terminal, not the current one\n"
	    "  -p  start at this order rather than the first\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	struct options opt;
	const char *outtty = NULL;
	uint64_t inc;
	uint32_t voices;
	bool listing = false, ok = true;
	char *end;
	int ch, fd, crossfeed = CROSSFEED;

	memset(&opt, 0, sizeof(opt));
	opt.outfd = STDOUT_FILENO;

	while ((ch = getopt(argc, argv, "c:d:iLlo:p:")) != -1) {
		switch (ch) {
		case 'c':
			crossfeed = (int)strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    crossfeed < 0 || crossfeed > 100)
				usage();
			break;
		case 'd':
			if (!display_select(optarg))
				usage();
			break;
		case 'i':
			opt.ignore = true;
			break;
		case 'L':
			listing = true;
			break;
		case 'l':
			opt.loop = true;
			break;
		case 'o':
			outtty = optarg;
			break;
		case 'p':
			opt.start = (int)strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    opt.start < 0 || opt.start >= MOD_ORDERS)
				usage();
			break;
		default:
			usage();
		}
	}
	if (optind == argc)
		usage();

	if (listing) {
		for (; optind < argc; optind++)
			if (!list(argv[optind]))
				ok = false;
		return ok ? 0 : 1;
	}

	/* O_NOCTTY: naming a console here must not make it this
	 * session's controlling terminal. */
	if (outtty != NULL &&
	    (opt.outfd = open(outtty, O_RDWR | O_NOCTTY)) == -1)
		err(1, "%s", outtty);

	/* No SA_RESTART: a signal has to end the wait in poll(). */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if ((fd = open(DEVICE, O_RDWR)) == -1)
		err(1, "%s", DEVICE);
	if (ioctl(fd, PENAUDIO_GETINFO, &info) == -1)
		err(1, "GETINFO");

	/* Both worked out once, so a tick costs no percentages and no
	 * floating point: the machine has no FPU. */
	inc = ((uint64_t)PAULA_HZ << 16) / info.pi_rate;
	if (inc > UINT32_MAX)
		errx(1, "no increment fits a %u Hz device", info.pi_rate);
	paula_inc = (uint32_t)inc;
	gains_init(crossfeed);
	voices = MOD_CHANNELS;
	if (ioctl(fd, PENAUDIO_ALLOC, &voices) == -1)
		err(1, "ALLOC");
	if (voices < MOD_CHANNELS)
		errx(1, "%u of %d voices free; is /dev/audio in use?",
		    voices, MOD_CHANNELS);

	for (; optind < argc && !quit; optind++)
		if (!play(fd, argv[optind], &opt))
			ok = false;

	close(fd);
	return ok ? 0 : 1;
}

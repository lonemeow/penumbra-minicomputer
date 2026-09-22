/*
 * modplay: play tracker modules.
 *
 * The replayer turns a song into ticks, and this file plays them one
 * of two ways.  A song the audio device's voices can hold plays on
 * them, through /dev/pmusic: each tick becomes events — a period into
 * an increment, a volume and the voice's place in the stereo field into
 * a gain for each side — submitted a little ahead of the kernel's
 * clock, which then keeps the time.  The samples go into the device's
 * sample memory once, as the file stores them, so a sample's offset
 * there is its offset in the file's sample data.
 *
 * A song with more channels than the device has voices, or more
 * samples than its memory holds, is mixed in software (mixer.c)
 * instead, and written to /dev/audio, which then keeps the time.
 *
 * usage: modplay [-Lils] [-c crossfeed] [-p order] [-r rate] file ...
 */

#include <sys/types.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

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
#include <time.h>
#include <unistd.h>

#include "mixer.h"
#include "replay.h"

#define	VOICES_DEVICE	"/dev/pmusic"
#define	AUDIO_DEVICE	"/dev/audio"
#define	AUDIOCTL_DEVICE	"/dev/audioctl"

#define	LOOKAHEAD	50	/* ticks generated ahead of the clock */
#define	LOWAT		25	/* wake when fewer ticks than this remain */
#define	PEND_MAX	512
#define	EVENTS_PER_TICK	(REPLAY_VOICES + 1)	/* a voice each, and tempo */
/*
 * How often to look for the song's end once every tick is submitted:
 * the device signals when it wants more, but nothing when the last
 * tick has played, so that is found by asking.
 */
#define	POLL_MS		40

#define	OUT_FRAMES	1024	/* mixed frames written at a time */

struct player {
	int		fd;
	const struct song *song;
	struct replay	replay;

	uint32_t	gen;		/* next tick to generate */
	bool		done;		/* the final stop is generated */
	uint32_t	last;		/*  ... on this tick */
	struct penaudio_event pend[PEND_MAX];
	uint32_t	npend;		/* generated, not yet accepted */

	/* What each voice was last told, so only changes are sent. */
	struct {
		bool		sounding;	/* started, and not since stopped */
		struct replay_region loop;	/* what follows its pass */
		uint32_t	inc;
		uint16_t	vol_l, vol_r;
	} voice[REPLAY_VOICES];
	int		bpm;
	int		start_bpm;	/* the first tick's, which starts the clock */
};

/* What the command line asked for, beyond which files to play. */
struct options {
	bool		ignore;		/* play despite effects modplay skips */
	bool		loop;
	int		start;		/* the order to begin at */
	int		crossfeed;	/* percent; -1: what suits the song */
	bool		mix;		/* mix even a song the voices can hold */
	bool		write_mix;	/* write the mix rather than map the buffer */
	uint32_t	rate;		/* to mix at; 0: half the device's */
};

static struct penaudio_info info;
static volatile sig_atomic_t quit;

/* INC at a period of 1, for the device's rate and the song's clock:
 * 16.16, like INC. */
static uint32_t clock_inc;

/* The song's, or what the command line asked for. */
static int crossfeed;

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
	uint32_t inc = (clock_inc + period / 2) / period;

	if (inc < PENAUDIO_INC_MIN)
		return PENAUDIO_INC_MIN;
	if (inc > PENAUDIO_INC_MAX)
		return PENAUDIO_INC_MAX;
	return inc;
}

/*
 * Each side's gain for a voice, by its volume (0..64) and pan.  The
 * pan shares the gain out between the sides, and crossfeed narrows
 * the field: a voice panned to one side alone gives the other side
 * the crossfeed's share of what its own side gets.
 *
 * pmusic gains run from 0x0000, silent, to 0xFFFF, unity, and the
 * device saturates a side's sum once, after mixing.  Every voice
 * arrives at full 16-bit scale, since each 8-bit sample is shifted up,
 * so the gains alone decide whether a loud passage clips: a voice's
 * two sides always add up to half scale at volume 64, so the Amiga's
 * four channels, two to a side, sum to exactly full scale on either
 * side at full volume, whatever the crossfeed.
 */
static void
voice_gain(int volume, int pan, uint16_t *left, uint16_t *right)
{
	uint32_t gain = volume * 0xFFFF / 64 / 2;
	uint32_t whole = 2 * REPLAY_PAN * (100 + crossfeed);
	uint32_t centre = REPLAY_PAN * (100 + crossfeed);
	int32_t spread = pan * (100 - crossfeed);

	*left = gain * (centre - spread) / whole;
	*right = gain * (centre + spread) / whole;
}

static bool
region_eq(const struct replay_region *a, const struct replay_region *b)
{
	return a->offset == b->offset && a->length == b->length;
}

/* One voice's event for a tick: whatever changed since the last. */
static void
voice_event(struct player *p, int ch, const struct replay_voice *v,
    struct penaudio_event *e)
{
	uint16_t vol_l, vol_r;
	uint32_t inc;
	bool started = false;

	if (v->instrument == 0 || v->period == 0)
		return;

	if (v->trigger) {
		/* A start with nothing in either pass is a sound that
		 * cannot begin, so what was playing falls silent. */
		if (v->play.length == 0 && v->loop.length == 0) {
			e->pe_set |= PENAUDIO_SET_STOP;
			p->voice[ch].sounding = false;
			return;
		}
		e->pe_set |= PENAUDIO_SET_PLAY;
		e->pe_format = PENAUDIO_FMT_S8;
		e->pe_play.pr_offset = v->play.offset;
		e->pe_play.pr_length = v->play.length;
		e->pe_loop.pr_offset = v->loop.offset;
		e->pe_loop.pr_length = v->loop.length;
		p->voice[ch].sounding = true;
		p->voice[ch].loop = v->loop;
		started = true;
	} else if (v->relatch && p->voice[ch].sounding &&
	    !region_eq(&v->loop, &p->voice[ch].loop)) {
		e->pe_set |= PENAUDIO_SET_LOOP;
		e->pe_format = PENAUDIO_FMT_S8;
		e->pe_loop.pr_offset = v->loop.offset;
		e->pe_loop.pr_length = v->loop.length;
		p->voice[ch].loop = v->loop;
	}

	inc = period_inc(v->period);
	if (started || inc != p->voice[ch].inc) {
		e->pe_set |= PENAUDIO_SET_INC;
		e->pe_inc = inc;
		p->voice[ch].inc = inc;
	}
	voice_gain(v->volume, v->pan, &vol_l, &vol_r);
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
		for (ch = 0; ch < p->song->channels; ch++) {
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
	for (ch = 0; ch < p->song->channels; ch++) {
		memset(&ev[n], 0, sizeof(ev[n]));
		ev[n].pe_tick = tick;
		ev[n].pe_voice = ch;
		voice_event(p, ch, &t.voice[ch], &ev[n]);
		if (ev[n].pe_set != 0)
			n++;
	}

	/* Kept rather than read back later: by the time the clock starts,
	 * feed() has run well past this tick and bpm is that tick's. */
	if (tick == 0)
		p->start_bpm = t.bpm;
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

/* ── Songs ───────────────────────────────────────────────────── */

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
	static struct replay_cue cue[SONG_ORDERS];
	struct song song;
	const char *error;
	uint8_t *buf;
	size_t len;
	int n, i;

	if ((buf = slurp(path, &len)) == NULL)
		return false;
	if ((error = song_parse(&song, buf, len)) != NULL) {
		warnx("%s: %s", path, error);
		free(buf);
		return false;
	}

	printf("%s: \"%s\" %s, %d orders\n", path, song.title, song.kind,
	    song.length);
	n = replay_scan(&song, cue, SONG_ORDERS);
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

	song_free(&song);
	free(buf);
	return true;
}

/* ── Playing on the device's voices ──────────────────────────── */

/*
 * Claim the device's voices for a song, if it fits them: a voice for
 * each channel, and room for its samples.  Returns the device, or -1
 * when the song has to be mixed in software instead.
 */
static int
voices_open(const struct song *s)
{
	uint32_t voices = (uint32_t)s->channels;
	int fd;

	if ((fd = open(VOICES_DEVICE, O_RDWR)) == -1)
		return -1;
	if (ioctl(fd, PENAUDIO_GETINFO, &info) == -1)
		err(1, "GETINFO");
	if (info.pi_nvoices < voices || s->sampledata_size > info.pi_maxmem) {
		close(fd);
		return -1;
	}
	/* Fewer are granted while /dev/audio holds one of them. */
	if (ioctl(fd, PENAUDIO_ALLOC, &voices) == -1)
		err(1, "ALLOC");
	if (voices < (uint32_t)s->channels) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool
play_voices(int fd, const char *path, const struct song *s,
    const struct options *opt)
{
	static struct player p;
	struct penaudio_clock pc;
	struct pollfd pfd;
	struct penaudio_status st;
	uint64_t inc;
	bool more;

	/* Worked out once, so a tick costs no floating point: the machine
	 * has no FPU. */
	inc = ((uint64_t)s->clock << 16) / info.pi_rate;
	if (inc > UINT32_MAX) {
		warnx("%s: no increment fits its clock on a %u Hz device",
		    path, info.pi_rate);
		return false;
	}
	clock_inc = (uint32_t)inc;

	if (ioctl(fd, PENAUDIO_RESET) == -1)
		err(1, "RESET");
	if (ioctl(fd, PENAUDIO_SETMEM, &s->sampledata_size) == -1)
		err(1, "SETMEM");
	if (s->sampledata_size != 0 &&
	    pwrite(fd, s->sampledata, s->sampledata_size, 0) !=
	    (ssize_t)s->sampledata_size)
		err(1, "loading samples");

	memset(&p, 0, sizeof(p));
	p.fd = fd;
	p.song = s;
	p.bpm = 125;		/* ProTracker's; the song may change it */
	replay_init(&p.replay, s, opt->start, opt->loop);

	/*
	 * The clock starts at the first tick's tempo, which is known once
	 * that tick is generated — unless the song stopped before its
	 * first tick, leaving nothing to time.
	 */
	more = feed(&p);
	if (!(p.done && p.last == 0)) {
		pc.pc_period_us = REPLAY_TICK_US(p.start_bpm);
		pc.pc_lowat = LOWAT;
		if (ioctl(fd, PENAUDIO_START, &pc) == -1)
			err(1, "START");
	}

	pfd.fd = fd;
	pfd.events = POLLOUT;
	while (!quit) {
		/* Once everything is submitted, only the timeout wakes. */
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
	}

	/* Silence at once, rather than letting queued ticks play out. */
	if (ioctl(fd, PENAUDIO_RESET) == -1)
		err(1, "RESET");
	return true;
}

/* ── Mixing in software ──────────────────────────────────────── */

/* The fastest rate the device offers, which its hardware runs at
 * unless told otherwise; 0 if it offers none. */
static uint32_t
native_rate(int ctl)
{
	audio_format_query_t q;
	uint32_t best = 0, i;

	for (q.index = 0; ioctl(ctl, AUDIO_QUERYFORMAT, &q) == 0; q.index++) {
		if (!(q.fmt.mode & AUMODE_PLAY))
			continue;
		/* A type of 0 is a range, lowest to highest; otherwise
		 * the type counts the rates listed. */
		if (q.fmt.frequency_type == 0) {
			if (q.fmt.frequency[1] > best)
				best = q.fmt.frequency[1];
			continue;
		}
		for (i = 0; i < q.fmt.frequency_type &&
		    i < AUFMT_MAX_FREQUENCIES; i++)
			if (q.fmt.frequency[i] > best)
				best = q.fmt.frequency[i];
	}
	return best;
}

/* Returns false if the mix could not be written, or a signal ended
 * the song. */
static bool
write_frames(int fd, const int16_t *out, uint32_t frames)
{
	const char *p = (const char *)out;
	size_t left = (size_t)frames * 2 * sizeof(out[0]);
	ssize_t n;

	while (left > 0) {
		if ((n = write(fd, p, left)) == -1) {
			if (errno == EINTR && !quit)
				continue;
			if (errno != EINTR)
				warn("%s", AUDIO_DEVICE);
			return false;
		}
		p += n;
		left -= (size_t)n;
	}
	return true;
}

/*
 * The mix, written to the device a buffer at a time.
 *
 * Every write copies the mix into the kernel's ring while this process
 * holds the track, and the kernel's mixer passes over a track a process
 * holds, putting out a block of silence when it does.  That costs a
 * dropout now and then, at the rate the copies take; it is what a
 * device whose buffer cannot be mapped leaves.
 */
static bool
mix_written(int fd, struct mixer *mx, struct replay *r)
{
	static int16_t out[OUT_FRAMES * 2];
	struct replay_tick t;
	uint32_t n = 0, got;
	bool ok = true;

	while (ok && !quit && replay_next(r, &t)) {
		mixer_tick(mx, &t);
		while (ok && (got = mixer_mix(mx, out + 2 * n,
		    OUT_FRAMES - n)) > 0) {
			n += got;
			if (n == OUT_FRAMES) {
				ok = write_frames(fd, out, n);
				n = 0;
			}
		}
	}
	if (ok && !quit && n > 0)
		ok = write_frames(fd, out, n);

	/* Let the end of the song play out, unless a signal ended it. */
	if (ok && !quit && ioctl(fd, AUDIO_DRAIN) == -1 && errno != EINTR)
		warn("%s", AUDIO_DEVICE);
	if (quit)
		(void)ioctl(fd, AUDIO_FLUSH);
	return ok;
}

/*
 * The mix, straight into the device's own ring.
 *
 * Mapping the ring leaves nothing to copy and nothing held: the kernel's
 * mixer takes the track's lock itself when it reads a block, so it never
 * passes the track over.  What it does instead is read the ring whether
 * or not anything has written it, so the mix has to stay ahead of it:
 * AUDIO_GETOOFFS says how far it has read, and everything from there to
 * where the mix has reached is what there is to fill.
 *
 * Returns false if the device's buffer cannot be mapped, for the caller
 * to write the mix instead.
 */
static bool
mix_mapped(int fd, struct mixer *mx, struct replay *r, uint32_t rate)
{
	struct replay_tick t;
	struct audio_info ai;
	audio_offset_t ao;
	struct timespec nap;
	uint8_t *ring;
	size_t maplen;
	long page = sysconf(_SC_PAGESIZE);
	uint32_t capacity, block, written = 0, end = 0, late = 0;
	bool playing = true;

	if (ioctl(fd, AUDIO_GETBUFINFO, &ai) == -1) {
		warn("GETBUFINFO");
		return false;
	}
	capacity = ai.play.buffer_size - ai.play.buffer_size % MIXER_FRAME_BYTES;
	block = ai.blocksize;
	if (page <= 0 || capacity < 2 * block || block == 0)
		return false;
	maplen = ((size_t)capacity + (size_t)page - 1) / (size_t)page *
	    (size_t)page;
	ring = mmap(NULL, maplen, PROT_WRITE, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED) {
		warn("cannot map %s's buffer", AUDIO_DEVICE);
		return false;
	}

	/* Half a block: short enough that the ring is filled well before
	 * the device reads that far, and long enough to cost little.
	 * Worked out in 64 bits, since a block's worth of nanoseconds
	 * does not fit a long here. */
	nap.tv_sec = 0;
	nap.tv_nsec = (long)((uint64_t)(block / MIXER_FRAME_BYTES) *
	    500000000u / rate);

	while (!quit) {
		int32_t unread;
		uint32_t room;

		if (ioctl(fd, AUDIO_GETOOFFS, &ao) == -1) {
			warn("GETOOFFS");
			break;
		}
		/*
		 * What the mix has put in and the device has not read yet
		 * has to stay; the rest is free.  The device starts
		 * reading as soon as the ring is mapped, and reads on
		 * whether or not the mix keeps up, so it can be past the
		 * mix — at the start, and again if ever the mix falls
		 * behind.  Then it has played what the ring held there,
		 * and the mix takes up again where the device is.
		 */
		unread = (int32_t)(written - ao.samples);
		if (unread < 0) {
			written = ao.samples;
			unread = 0;
			late++;
		}
		room = capacity - (uint32_t)unread;
		while (room >= MIXER_FRAME_BYTES) {
			uint32_t at = written % capacity, n;

			n = mixer_mix_ring(mx, ring, capacity, at, room);
			if (n == 0) {
				/* The tick is mixed: take the next, and
				 * once the song is over, leave silence
				 * behind it rather than the mix again. */
				if (playing && replay_next(r, &t)) {
					mixer_tick(mx, &t);
					continue;
				}
				if (playing) {
					playing = false;
					end = written;
				}
				n = room < capacity - at ? room : capacity - at;
				memset(ring + at, 0, n);
			}
			written += n;
			room -= n;
		}
		/* The song is played once the device has read all of it,
		 * which is a difference that has not run backwards. */
		if (!playing && ao.samples - end < capacity)
			break;
		if (nanosleep(&nap, NULL) == -1 && errno != EINTR) {
			warn("nanosleep");
			break;
		}
	}

	/* A signal stops the song at once: what the device has still to
	 * read falls silent rather than playing out. */
	if (quit)
		memset(ring, 0, capacity);

	munmap(ring, maplen);
	if (late != 0)
		warnx("the mix fell behind the device %u times", late);
	return true;
}

static bool
play_mixed(const struct song *s, const struct options *opt)
{
	static struct mixer mx;
	static struct replay r;
	struct audio_info hw, ai;
	uint32_t rate = opt->rate;
	bool restore = false, ok = true;
	int ctl, fd;

	if ((ctl = open(AUDIOCTL_DEVICE, O_RDWR)) == -1) {
		warn("%s", AUDIOCTL_DEVICE);
		return false;
	}
	if (rate == 0 && (rate = native_rate(ctl) / 2) == 0) {
		warnx("%s offers no rate to play at", AUDIOCTL_DEVICE);
		close(ctl);
		return false;
	}

	/*
	 * Run the hardware at the mixing rate, so that its voice resamples
	 * the mix for nothing, and put it back afterwards.  It cannot
	 * change while anything has the device open, nor to a rate it
	 * cannot run at; audio(4) then converts the mix itself.
	 */
	if (ioctl(ctl, AUDIO_GETFORMAT, &hw) == -1)
		err(1, "GETFORMAT");
	if (hw.play.sample_rate != rate) {
		ai = hw;
		ai.play.sample_rate = rate;
		if (ioctl(ctl, AUDIO_SETFORMAT, &ai) == 0)
			restore = true;
		else
			warn("the device stays at %u Hz, so the kernel converts "
			    "the mix", hw.play.sample_rate);
	}

	if ((fd = open(AUDIO_DEVICE, O_WRONLY)) == -1) {
		warn("%s", AUDIO_DEVICE);
		ok = false;
		goto done;
	}
	AUDIO_INITINFO(&ai);
	ai.play.sample_rate = rate;
	ai.play.channels = 2;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	if (ioctl(fd, AUDIO_SETINFO, &ai) == -1)
		err(1, "SETINFO");

	printf("  mixing %d channels at %u Hz%s\n", s->channels, rate,
	    opt->write_mix ? ", written to the device" : "");
	mixer_init(&mx, s, rate, crossfeed);
	replay_init(&r, s, opt->start, opt->loop);
	if (opt->write_mix || !mix_mapped(fd, &mx, &r, rate))
		ok = mix_written(fd, &mx, &r);
	close(fd);

done:
	if (restore && ioctl(ctl, AUDIO_SETFORMAT, &hw) == -1)
		warn("could not put the device back to %u Hz",
		    hw.play.sample_rate);
	close(ctl);
	return ok;
}

/* ── Files ───────────────────────────────────────────────────── */

/* Returns false if the file could not be played. */
static bool
play(const char *path, const struct options *opt)
{
	struct song song;
	const char *error, *unsupported;
	uint8_t *buf;
	size_t len;
	bool ok;
	int fd = -1;

	if ((buf = slurp(path, &len)) == NULL)
		return false;
	if ((error = song_parse(&song, buf, len)) != NULL) {
		warnx("%s: %s", path, error);
		free(buf);
		return false;
	}

	ok = false;
	if (opt->start >= song.length) {
		warnx("%s: has %d orders, so -p %d starts past its end", path,
		    song.length, opt->start);
		goto done;
	}

	unsupported = song_unsupported(&song);
	if (unsupported != NULL && !opt->ignore) {
		warnx("%s: uses %s, which modplay does not play; "
		    "-i plays it without", path, unsupported);
		goto done;
	}
	if (unsupported != NULL)
		printf("  playing without %s\n", unsupported);

	crossfeed = opt->crossfeed >= 0 ? opt->crossfeed : song.crossfeed;
	if (!opt->mix)
		fd = voices_open(&song);
	if (fd != -1) {
		ok = play_voices(fd, path, &song, opt);
		close(fd);
	} else
		ok = play_mixed(&song, opt);

done:
	song_free(&song);
	free(buf);
	return ok;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: modplay [-Lilsw] [-c crossfeed] [-p order] [-r rate] "
	    "file ...\n"
	    "  -c  percent of each channel's own side that the other side "
	    "gets:\n"
	    "      0 pans hard, as the Amiga does, 100 is mono; default %d\n"
	    "  -i  play a module even if it uses effects modplay ignores\n"
	    "  -L  list where each file's cues start instead of playing\n"
	    "  -l  loop each song rather than stopping at its end\n"
	    "  -p  start at this order rather than the first\n"
	    "  -r  mix at this rate, where a song is mixed in software;\n"
	    "      default half the device's own\n"
	    "  -s  mix in software even a song the device's voices could "
	    "play\n"
	    "  -w  write the mix to the device rather than mixing into its "
	    "buffer\n",
	    MOD_CROSSFEED);
	exit(2);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	struct options opt;
	unsigned long rate;
	bool listing = false, ok = true;
	char *end;
	int ch;

	memset(&opt, 0, sizeof(opt));
	opt.crossfeed = -1;

	while ((ch = getopt(argc, argv, "c:iLlp:r:sw")) != -1) {
		switch (ch) {
		case 'c':
			opt.crossfeed = (int)strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    opt.crossfeed < 0 || opt.crossfeed > 100)
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
		case 'p':
			opt.start = (int)strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    opt.start < 0 || opt.start >= SONG_ORDERS)
				usage();
			break;
		case 'r':
			rate = strtoul(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' ||
			    rate < 1000 || rate > 192000)
				usage();
			opt.rate = (uint32_t)rate;
			break;
		case 's':
			opt.mix = true;
			break;
		case 'w':
			opt.write_mix = true;
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

	/* No SA_RESTART: a signal has to end the wait in poll() or
	 * write(). */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	for (; optind < argc && !quit; optind++)
		if (!play(argv[optind], &opt))
			ok = false;
	return ok ? 0 : 1;
}

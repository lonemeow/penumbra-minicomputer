/*
 * pmusic-test: exercise /dev/pmusic, the audio device's voice interface
 * (<machine/penaudio.h>; doc/system/netbsd/design-notes.md, "Audio
 * Driver").
 *
 * A test either checks the interface's contract and prints PASS or
 * FAIL, or plays something whose sound is the check and says what to
 * listen for; most do both.  Every test opens a session of its own, so
 * each also relies on the previous one's close having released
 * everything.
 *
 * usage: pmusic-test [test ...]		(no argument runs them all)
 */

#include <sys/param.h>
#include <sys/ioctl.h>

#include <machine/penaudio.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	DEVICE		"/dev/pmusic"

static struct penaudio_info info;
static int failures;

static void
check(bool ok, const char *fmt, ...)
{
	va_list ap;

	printf("  %s  ", ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	if (!ok)
		failures++;
}

static void
listen(const char *what)
{
	printf("  listen: %s\n", what);
	fflush(stdout);
}

static uint64_t
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/* ── Device ──────────────────────────────────────────────────── */

static int
open_dev(void)
{
	int fd = open(DEVICE, O_RDWR);

	if (fd == -1 && errno == ENOENT)
		errx(1, "no %s; create it with: mknod %s c 43 0",
		    DEVICE, DEVICE);
	if (fd == -1)
		err(1, "%s", DEVICE);
	return fd;
}

static void
xioctl(int fd, unsigned long cmd, void *arg, const char *what)
{
	if (ioctl(fd, cmd, arg) == -1)
		err(1, "%s", what);
}

static void
status(int fd, struct penaudio_status *st)
{
	xioctl(fd, PENAUDIO_GETSTATUS, st, "GETSTATUS");
}

static uint32_t
alloc(int fd, uint32_t want)
{
	xioctl(fd, PENAUDIO_ALLOC, &want, "ALLOC");
	return want;
}

static void
start(int fd, uint32_t period_us, uint32_t lowat)
{
	struct penaudio_clock pc = { period_us, lowat };

	xioctl(fd, PENAUDIO_START, &pc, "START");
}

/*
 * SUBMIT.  Returns 0 or the call's errno; on success the count taken
 * and the status come back through the pointers that are not NULL.
 */
static int
submit(int fd, const struct penaudio_event *ev, uint32_t n,
    uint32_t through, uint32_t *acceptedp, struct penaudio_status *stp)
{
	struct penaudio_submit ps = {
		.ps_events = ev,
		.ps_eventsize = sizeof(*ev),
		.ps_nevents = n,
		.ps_through = through,
	};

	if (ioctl(fd, PENAUDIO_SUBMIT, &ps) == -1)
		return errno;
	if (acceptedp != NULL)
		*acceptedp = ps.ps_nevents;
	if (stp != NULL)
		*stp = ps.ps_status;
	return 0;
}

/* SUBMIT that must take everything offered. */
static void
submit_all(int fd, const struct penaudio_event *ev, uint32_t n,
    uint32_t through)
{
	uint32_t accepted;
	int error = submit(fd, ev, n, through, &accepted, NULL);

	if (error != 0)
		errc(1, error, "SUBMIT");
	if (accepted != n)
		errx(1, "SUBMIT took %u of %u events", accepted, n);
}

/* Wait until the clock reaches `tick`, for at most timeout_ms. */
static bool
wait_tick(int fd, uint32_t tick, unsigned timeout_ms)
{
	struct penaudio_status st;
	uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000;

	do {
		status(fd, &st);
		if (st.pst_tick >= tick)
			return true;
		sleep_ms(2);
	} while (now_us() < deadline);
	return false;
}

/* ── Sample memory ───────────────────────────────────────────── */

#define	CYCLE		64	/* frames in a single-cycle waveform */

#define	OFF_SINE	0				/* S8 */
#define	OFF_SQUARE	(OFF_SINE + CYCLE)		/* S8 */
#define	OFF_SILENCE	(OFF_SQUARE + CYCLE)		/* S8, zeros */
#define	OFF_SAW16	(OFF_SILENCE + CYCLE)		/* S16 */
#define	OFF_STEREO	(OFF_SAW16 + CYCLE * 2)		/* S16 stereo */
#define	OFF_PLUCK	(OFF_STEREO + CYCLE * 4)	/* S8, one-shot */
#define	PLUCK_RATE	16000	/* frames per second it is written for */
#define	PLUCK_FRAMES	(PLUCK_RATE / 2)
#define	MEMSIZE		(OFF_PLUCK + PLUCK_FRAMES)

static void
load_samples(int fd)
{
	static uint32_t words[(MEMSIZE + 3) / 4];
	uint8_t *mem = (uint8_t *)words;
	int16_t *saw = (int16_t *)(mem + OFF_SAW16);
	int16_t *stereo = (int16_t *)(mem + OFF_STEREO);
	uint32_t size = MEMSIZE;
	int i;

	for (i = 0; i < CYCLE; i++) {
		double ph = 2 * M_PI * i / CYCLE;

		mem[OFF_SINE + i] = (uint8_t)(int8_t)lrint(120 * sin(ph));
		mem[OFF_SQUARE + i] = (uint8_t)(int8_t)(i < CYCLE / 2 ? 90 : -90);
		saw[i] = (int16_t)(-30000 + 60000 * i / (CYCLE - 1));
		stereo[2 * i] = (int16_t)lrint(30000 * sin(ph));
		stereo[2 * i + 1] = (int16_t)(i < CYCLE / 2 ? 22000 : -22000);
	}
	for (i = 0; i < PLUCK_FRAMES; i++) {
		double t = (double)i / PLUCK_RATE;

		mem[OFF_PLUCK + i] = (uint8_t)(int8_t)lrint(
		    120 * exp(-6 * t) * sin(2 * M_PI * 440 * t));
	}

	xioctl(fd, PENAUDIO_SETMEM, &size, "SETMEM");
	if (pwrite(fd, mem, MEMSIZE, 0) != MEMSIZE)
		err(1, "pwrite samples");
}

/* The increment that plays a waveform of `frames` frames per cycle at
 * `hz` cycles per second. */
static uint32_t
inc_for(double hz, double frames)
{
	return (uint32_t)lrint(hz * frames * 65536.0 / info.pi_rate);
}

static double
note_hz(int midi)
{
	return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

/* ── Events ──────────────────────────────────────────────────── */

#define	GAIN		0x5000	/* four voices at this stay under full scale */

static struct penaudio_event
ev_play(uint32_t tick, uint16_t voice, uint32_t inc, uint16_t vol_l,
    uint16_t vol_r, uint32_t format, uint32_t off, uint32_t len,
    uint32_t loop_off, uint32_t loop_len)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = voice,
		.pe_set = PENAUDIO_SET_INC | PENAUDIO_SET_VOL |
		    PENAUDIO_SET_PLAY,
		.pe_inc = inc,
		.pe_vol_l = vol_l,
		.pe_vol_r = vol_r,
		.pe_format = format,
		.pe_play = { off, len },
		.pe_loop = { loop_off, loop_len },
	};

	return e;
}

/* A looping single-cycle 8-bit waveform at `hz`. */
static struct penaudio_event
ev_tone(uint32_t tick, uint16_t voice, uint32_t off, double hz,
    uint16_t vol_l, uint16_t vol_r)
{
	return ev_play(tick, voice, inc_for(hz, CYCLE), vol_l, vol_r,
	    PENAUDIO_FMT_S8, off, CYCLE, off, CYCLE);
}

static struct penaudio_event
ev_loop(uint32_t tick, uint16_t voice, uint32_t off)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = voice,
		.pe_set = PENAUDIO_SET_LOOP,
		.pe_format = PENAUDIO_FMT_S8,
		.pe_loop = { off, CYCLE },
	};

	return e;
}

static struct penaudio_event
ev_vol(uint32_t tick, uint16_t voice, uint16_t vol_l, uint16_t vol_r)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = voice,
		.pe_set = PENAUDIO_SET_VOL,
		.pe_vol_l = vol_l,
		.pe_vol_r = vol_r,
	};

	return e;
}

static struct penaudio_event
ev_inc(uint32_t tick, uint16_t voice, uint32_t inc)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = voice,
		.pe_set = PENAUDIO_SET_INC,
		.pe_inc = inc,
	};

	return e;
}

static struct penaudio_event
ev_stop(uint32_t tick, uint16_t voice)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = voice,
		.pe_set = PENAUDIO_SET_STOP,
	};

	return e;
}

static struct penaudio_event
ev_period(uint32_t tick, uint32_t period_us)
{
	struct penaudio_event e = {
		.pe_tick = tick,
		.pe_voice = PENAUDIO_VOICE_NONE,
		.pe_set = PENAUDIO_SET_PERIOD,
		.pe_period_us = period_us,
	};

	return e;
}

/* ── Tests ───────────────────────────────────────────────────── */

static void
test_info(void)
{
	printf("info: what the device offers\n");
	printf("  rate %u Hz, %u voices (%u available), "
	    "sample memory up to %u bytes,\n"
	    "  %u queued events, ticks on a %u us grain\n",
	    info.pi_rate, info.pi_nvoices, info.pi_navail, info.pi_maxmem,
	    info.pi_maxevents, info.pi_tick_grain_us);
}

static void
test_api(void)
{
	static struct penaudio_event bad[] = {
		{ .pe_tick = 5, .pe_set = 0x40 },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_STOP | PENAUDIO_SET_PLAY,
		  .pe_format = PENAUDIO_FMT_S8 },
		{ .pe_tick = 5, .pe_voice = 0, .pe_set = PENAUDIO_SET_PERIOD,
		  .pe_period_us = 20000 },
		{ .pe_tick = 5, .pe_voice = PENAUDIO_VOICE_NONE,
		  .pe_set = PENAUDIO_SET_PERIOD | PENAUDIO_SET_VOL,
		  .pe_period_us = 20000 },
		{ .pe_tick = 5, .pe_voice = PENAUDIO_VOICE_NONE,
		  .pe_set = PENAUDIO_SET_PERIOD,
		  .pe_period_us = PENAUDIO_PERIOD_MIN_US - 1 },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_INC, .pe_inc = 0xFF },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_INC,
		  .pe_inc = 0x40001 },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_PLAY,
		  .pe_format = PENAUDIO_FMT_S8, .pe_play = { 4000, 200 } },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_PLAY,
		  .pe_format = PENAUDIO_FMT_S16, .pe_play = { 1, 64 } },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_PLAY,
		  .pe_format = 9, .pe_play = { 0, 64 } },
		{ .pe_tick = 5, .pe_set = PENAUDIO_SET_VOL },	/* voice set below */
	};
	static const char *const bad_what[] = {
		"an unknown SET bit",
		"STOP with PLAY",
		"PERIOD on a voice",
		"PERIOD with a voice field",
		"a period below the minimum",
		"an increment below the range",
		"an increment above the range",
		"a region past the end of sample memory",
		"a misaligned 16-bit region",
		"an unknown format",
		"a voice outside the allocation",
	};
	struct penaudio_status st, st2;
	struct penaudio_event ev[2], *big;
	struct penaudio_clock pc;
	struct penaudio_info info2;
	struct pollfd pfd;
	char buf[16], back[16];
	uint32_t n, nvoices, accepted, i;
	int fd, fd2, error;

	printf("api: the interface's contract (silent)\n");
	fd = open_dev();

	fd2 = open(DEVICE, O_RDWR);
	check(fd2 == -1 && errno == EBUSY, "a second open fails with EBUSY");
	if (fd2 != -1)
		close(fd2);

	check(info.pi_rate > 0 && info.pi_nvoices > 0 &&
	    info.pi_maxmem > 0 && info.pi_maxevents > 0 &&
	    info.pi_tick_grain_us > 0,
	    "GETINFO reports a rate, voices, memory, a queue and a grain");

	/* Sample memory. */
	n = info.pi_maxmem + 1;
	check(ioctl(fd, PENAUDIO_SETMEM, &n) == -1 && errno == EINVAL,
	    "SETMEM above pi_maxmem fails with EINVAL");
	n = 4096;
	xioctl(fd, PENAUDIO_SETMEM, &n, "SETMEM");
	memset(buf, 0x5A, sizeof(buf));
	check(pwrite(fd, buf, 16, 100) == 16 &&
	    pread(fd, back, 16, 100) == 16 && memcmp(buf, back, 16) == 0,
	    "pread returns what pwrite wrote");
	check(pwrite(fd, buf, 16, 4090) == 6,
	    "a write running past the end is short");
	errno = 0;
	check(pwrite(fd, buf, 16, 4096) == -1 && errno == ENOSPC,
	    "a write starting at the end fails with ENOSPC");
	check(pread(fd, back, 16, 4096) == 0,
	    "a read at the end returns end of file");
	check(lseek(fd, 200, SEEK_SET) == 200 && write(fd, "X", 1) == 1 &&
	    pread(fd, back, 1, 200) == 1 && back[0] == 'X',
	    "write lands where lseek put the offset");

	/* Allocation. */
	nvoices = alloc(fd, info.pi_nvoices);
	check(nvoices == info.pi_navail,
	    "ALLOC grants the %u voices GETINFO said were available",
	    info.pi_navail);
	status(fd, &st);
	check(st.pst_room == info.pi_maxevents && st.pst_tick == 0 &&
	    st.pst_through == 0 && st.pst_sounding == 0,
	    "a fresh session is silent, with an empty queue at tick 0");

	/* A refused first event fails the call and changes nothing. */
	bad[__arraycount(bad) - 1].pe_voice = nvoices;
	for (i = 0; i < __arraycount(bad); i++) {
		error = submit(fd, &bad[i], 1, 10, NULL, NULL);
		status(fd, &st2);
		check(error == EINVAL && st2.pst_room == st.pst_room &&
		    st2.pst_through == st.pst_through,
		    "refuses %s, changing nothing", bad_what[i]);
	}
	ev[0] = ev_vol(5, 0, GAIN, GAIN);
	error = submit(fd, ev, 1, 10, NULL, NULL);
	ev[0].pe_tick = 3;
	check(error == 0 && submit(fd, ev, 1, 10, NULL, NULL) == EINVAL,
	    "refuses an event before one already queued");
	{
		struct penaudio_submit ps = {
			.ps_events = ev, .ps_eventsize = sizeof(ev[0]) - 4,
			.ps_nevents = 1,
		};

		check(ioctl(fd, PENAUDIO_SUBMIT, &ps) == -1 &&
		    errno == EINVAL, "refuses the wrong ps_eventsize");
	}
	xioctl(fd, PENAUDIO_RESET, NULL, "RESET");

	/* A batch stopping short: the valid prefix, and a horizon that
	 * stops before the first event not taken. */
	ev[0] = ev_vol(5, 0, GAIN, GAIN);
	ev[1] = bad[0];
	error = submit(fd, ev, 2, 10, &accepted, &st2);
	check(error == 0 && accepted == 1,
	    "takes the valid prefix of a batch (%u of 2)", accepted);
	check(st2.pst_through == 4,
	    "and moves the horizon only to the tick before the refused one "
	    "(%u)", st2.pst_through);

	/* Busy while anything is queued. */
	n = 1;
	check(ioctl(fd, PENAUDIO_ALLOC, &n) == -1 && errno == EBUSY,
	    "ALLOC fails with EBUSY while events are queued");
	n = 4096;
	check(ioctl(fd, PENAUDIO_SETMEM, &n) == -1 && errno == EBUSY,
	    "SETMEM fails with EBUSY while events are queued");
	xioctl(fd, PENAUDIO_RESET, NULL, "RESET");
	status(fd, &st2);
	check(st2.pst_room == info.pi_maxevents && st2.pst_tick == 0 &&
	    st2.pst_through == 0,
	    "RESET empties the queue and returns the clock to 0");

	/* A batch larger than the queue. */
	big = calloc(info.pi_maxevents + 8, sizeof(*big));
	if (big == NULL)
		err(1, "calloc");
	for (i = 0; i < info.pi_maxevents + 8; i++)
		big[i] = ev_vol(100 + i / 4, 0, GAIN, GAIN);
	error = submit(fd, big, info.pi_maxevents + 8, 100000, &accepted,
	    &st2);
	check(error == 0 && accepted == info.pi_maxevents &&
	    st2.pst_room == 0,
	    "a batch larger than the queue fills it (%u taken)", accepted);
	check(st2.pst_through == 100 + info.pi_maxevents / 4 - 1,
	    "and declares only the ticks it wholly holds (%u)",
	    st2.pst_through);
	free(big);
	xioctl(fd, PENAUDIO_RESET, NULL, "RESET");

	/* An immediate event applies during the call. */
	ev[0] = ev_play(0, 0, 0x10000, GAIN, GAIN, PENAUDIO_FMT_S8,
	    1024, 64, 1024, 64);	/* zeros: sounding, silently */
	submit_all(fd, ev, 1, 0);
	status(fd, &st2);
	check((st2.pst_sounding & 1) != 0,
	    "an event for a tick already applied plays at once");
	n = 4096;
	check(ioctl(fd, PENAUDIO_SETMEM, &n) == -1 && errno == EBUSY,
	    "SETMEM fails with EBUSY while a voice sounds");
	ev[0] = ev_stop(0, 0);
	submit_all(fd, ev, 1, 0);
	status(fd, &st2);
	check(st2.pst_sounding == 0 && st2.pst_underruns == 0,
	    "STOP silences it, and a loop never counted an underrun");

	/* The clock. */
	pfd.fd = fd;
	pfd.events = POLLOUT;
	check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT),
	    "a stopped clock with room polls writable");
	pc.pc_period_us = PENAUDIO_PERIOD_MIN_US - 1;
	pc.pc_lowat = 1;
	check(ioctl(fd, PENAUDIO_START, &pc) == -1 && errno == EINVAL,
	    "START below the minimum period fails with EINVAL");
	pc.pc_period_us = 20000;
	check(ioctl(fd, PENAUDIO_START, &pc) == 0, "START");
	check(ioctl(fd, PENAUDIO_START, &pc) == -1 && errno == EBUSY,
	    "a second START fails with EBUSY");
	xioctl(fd, PENAUDIO_RESET, NULL, "RESET");

	check(ioctl(fd, _IO('Y', 99)) == -1 && errno == ENOTTY,
	    "an unknown command fails with ENOTTY");

	close(fd);
	fd = open_dev();
	xioctl(fd, PENAUDIO_GETINFO, &info2, "GETINFO");
	check(info2.pi_navail == info.pi_navail,
	    "closing released the voices (%u available)", info2.pi_navail);
	close(fd);
}

static void
test_tone(void)
{
	struct penaudio_event ev;
	struct penaudio_status st;
	int fd;

	printf("tone: one looping voice, no clock\n");
	fd = open_dev();
	load_samples(fd);
	alloc(fd, 1);

	listen("a steady 440 Hz sine for 2 s, centred");
	ev = ev_tone(0, 0, OFF_SINE, 440, GAIN, GAIN);
	submit_all(fd, &ev, 1, 0);
	sleep_ms(2000);
	status(fd, &st);
	check(st.pst_sounding == 1 && st.pst_underruns == 0,
	    "it sounds throughout, with no underrun");

	ev = ev_stop(0, 0);
	submit_all(fd, &ev, 1, 0);
	close(fd);
}

static void
test_formats(void)
{
	struct penaudio_event ev;
	struct penaudio_status st;
	int fd;

	printf("formats: each sample format in turn\n");
	fd = open_dev();
	load_samples(fd);
	alloc(fd, 1);

	listen("1 s each: sine (8-bit), saw (16-bit), then sine left and "
	    "square right (16-bit stereo), all at 220 Hz");
	ev = ev_tone(0, 0, OFF_SINE, 220, GAIN, GAIN);
	submit_all(fd, &ev, 1, 0);
	sleep_ms(1000);
	ev = ev_play(0, 0, inc_for(220, CYCLE), GAIN, GAIN,
	    PENAUDIO_FMT_S16, OFF_SAW16, CYCLE * 2, OFF_SAW16, CYCLE * 2);
	submit_all(fd, &ev, 1, 0);
	sleep_ms(1000);
	ev = ev_play(0, 0, inc_for(220, CYCLE), GAIN, GAIN,
	    PENAUDIO_FMT_S16_STEREO, OFF_STEREO, CYCLE * 4, OFF_STEREO,
	    CYCLE * 4);
	submit_all(fd, &ev, 1, 0);
	sleep_ms(1000);

	status(fd, &st);
	check(st.pst_underruns == 0, "no underrun");
	close(fd);
}

static void
test_oneshot(void)
{
	struct penaudio_event ev;
	struct penaudio_status st;
	uint64_t t0, elapsed;
	int fd;

	printf("oneshot: a sound that ends by itself\n");
	fd = open_dev();
	load_samples(fd);
	alloc(fd, 1);

	listen("one plucked 440 Hz note, 0.5 s, dying away");
	ev = ev_play(0, 0, inc_for(PLUCK_RATE, 1), GAIN, GAIN,
	    PENAUDIO_FMT_S8, OFF_PLUCK, PLUCK_FRAMES, 0, 0);
	t0 = now_us();
	submit_all(fd, &ev, 1, 0);
	do {
		sleep_ms(10);
		status(fd, &st);
		elapsed = now_us() - t0;
	} while (st.pst_sounding != 0 && elapsed < 3000000);

	check(st.pst_sounding == 0, "the voice falls silent on its own");
	check(elapsed > 400000 && elapsed < 800000,
	    "after the sample's length (%llu ms of 500)",
	    (unsigned long long)elapsed / 1000);
	check(st.pst_underruns == 0,
	    "a sound running out is not an underrun");
	close(fd);
}

static void
test_chord(void)
{
	static const int notes[] = { 60, 64, 67, 72 };	/* C E G C */
	struct penaudio_event ev[4];
	struct penaudio_status st;
	uint32_t nv, v;
	int fd;

	printf("chord: every voice started by one call\n");
	fd = open_dev();
	load_samples(fd);
	nv = alloc(fd, 4);

	listen("a C major chord for 2 s, all notes starting together, "
	    "C and C left, E and G right");
	for (v = 0; v < nv; v++) {
		bool left = v == 0 || v == 3;	/* LRRL, as Paula */

		/* No first pass: the voice starts straight in its loop. */
		ev[v] = ev_play(0, v, inc_for(note_hz(notes[v]), CYCLE),
		    left ? GAIN : GAIN / 4, left ? GAIN / 4 : GAIN,
		    PENAUDIO_FMT_S8, OFF_SINE, 0, OFF_SINE, CYCLE);
	}
	submit_all(fd, ev, nv, 0);
	sleep_ms(2000);
	status(fd, &st);
	check(st.pst_sounding == (1U << nv) - 1 && st.pst_underruns == 0,
	    "all %u voices sound, with no underrun", nv);

	for (v = 0; v < nv; v++)
		ev[v] = ev_stop(0, v);
	submit_all(fd, ev, nv, 0);
	close(fd);
}

/*
 * The clock test's sequence: a note every ROW ticks, turn about across
 * the voices, each decaying tick by tick as a tracker's volume slide
 * would, and the tempo quickening halfway.  Stateless in the tick, so
 * any tick can be generated on its own.
 */
#define	ROW		6
#define	ROWS		32
#define	TEMPO_SLOW	20000	/* us per tick: 125 BPM */
#define	TEMPO_FAST	15000
#define	TEMPO_CHANGE	(ROWS / 2 * ROW)
#define	SEQ_LAST	(ROWS * ROW)
#define	MAXEV_PER_TICK	5

static uint32_t
seq_tick(uint32_t nvoices, uint32_t tick, struct penaudio_event *ev)
{
	static const int scale[] = { 60, 64, 67, 72, 76, 72, 67, 64 };
	uint32_t n = 0, v;

	if (tick == SEQ_LAST) {
		for (v = 0; v < nvoices; v++)
			ev[n++] = ev_stop(tick, v);
		return n;
	}
	if (tick == TEMPO_CHANGE)
		ev[n++] = ev_period(tick, TEMPO_FAST);

	for (v = 0; v < nvoices; v++) {
		uint32_t row = tick / ROW;
		bool left = v == 0 || v == 3;
		uint32_t struck, age;
		uint16_t gain;

		/* The last row at or before this tick that struck v. */
		if (row < v)
			continue;
		struck = row - (row - v) % nvoices;
		age = tick - struck * ROW;
		gain = (uint16_t)lrint(GAIN * pow(0.93, age));

		if (age == 0)
			ev[n++] = ev_tone(tick, v, OFF_SINE,
			    note_hz(scale[struck % 8]),
			    left ? gain : gain / 4, left ? gain / 4 : gain);
		else
			ev[n++] = ev_vol(tick, v, left ? gain : gain / 4,
			    left ? gain / 4 : gain);
	}
	return n;
}

#define	LOOKAHEAD	50	/* ticks generated ahead of the clock */
#define	LOWAT		25	/* wake when fewer ticks than this remain */
#define	PEND_MAX	512

struct feeder {
	int		fd;
	uint32_t	nvoices;
	uint32_t	gen;		/* next tick to generate */
	uint32_t	last;		/* the sequence's final tick */
	struct penaudio_event pend[PEND_MAX];
	uint32_t	npend;		/* generated, not yet accepted */
};

/*
 * Generate the sequence a little ahead of the clock and submit it.
 * Called once before START, then each time the device polls writable.
 * Returns false once the whole sequence has been accepted.
 */
static bool
feed(struct feeder *f)
{
	struct penaudio_status st;
	uint32_t accepted, target;
	int error;

	status(f->fd, &st);

	/* Every tick up to LOOKAHEAD past the clock, while a whole tick
	 * still fits in pend[]. */
	target = MIN(st.pst_tick + LOOKAHEAD, f->last);
	while (f->gen <= target && f->npend + MAXEV_PER_TICK <= PEND_MAX)
		f->npend += seq_tick(f->nvoices, f->gen++,
		    &f->pend[f->npend]);

	/* Every tick below gen is generated, so all of it is declared;
	 * the kernel holds the horizon back from anything it did not
	 * take. */
	error = submit(f->fd, f->pend, f->npend, f->gen - 1, &accepted, &st);
	if (error != 0)
		errc(1, error, "SUBMIT");
	f->npend -= accepted;
	memmove(f->pend, &f->pend[accepted], f->npend * sizeof(f->pend[0]));

	return f->gen <= f->last || f->npend > 0;
}

static void
test_clock(void)
{
	static struct feeder f;
	struct penaudio_status st;
	struct pollfd pfd;
	uint64_t t0, elapsed, expected;
	bool more, reached;
	int fd;

	printf("clock: a sequence fed ahead of the kernel's clock\n");
	fd = open_dev();
	load_samples(fd);

	memset(&f, 0, sizeof(f));
	f.fd = fd;
	f.nvoices = alloc(fd, 4);
	f.last = SEQ_LAST;

	listen("a rising and falling arpeggio, notes alternating left and "
	    "right, evenly spaced, quickening halfway");
	more = feed(&f);
	start(fd, TEMPO_SLOW, LOWAT);
	t0 = now_us();

	pfd.fd = fd;
	pfd.events = POLLOUT;
	while (more) {
		if (poll(&pfd, 1, 2000) != 1) {
			check(false, "the device polls writable while fed");
			break;
		}
		more = feed(&f);
	}
	reached = wait_tick(fd, SEQ_LAST, 10000);
	elapsed = now_us() - t0;
	expected = (uint64_t)TEMPO_CHANGE * TEMPO_SLOW +
	    (uint64_t)(SEQ_LAST - TEMPO_CHANGE) * TEMPO_FAST;

	status(fd, &st);
	check(reached, "the clock reaches the last tick");
	check(st.pst_stalls == 0, "it never waited at the horizon (%u)",
	    st.pst_stalls);
	check(st.pst_underruns == 0, "no underrun (%u)", st.pst_underruns);
	check(elapsed + 3 * info.pi_tick_grain_us >= expected &&
	    elapsed <= expected + 3 * info.pi_tick_grain_us,
	    "it took %llu ms, for %llu by the tempo",
	    (unsigned long long)elapsed / 1000,
	    (unsigned long long)expected / 1000);
	close(fd);
}

static void
test_stall(void)
{
	struct penaudio_event ev[16];
	struct penaudio_status st;
	uint32_t n, t, stalls;
	int fd;

	printf("stall: the clock waits at the horizon\n");
	fd = open_dev();
	load_samples(fd);
	alloc(fd, 1);

	listen("four notes, a 1 s gap in which the last one holds, then "
	    "four more");
	for (n = 0, t = 0; t <= 15; t += 5)
		ev[n++] = ev_tone(t, 0, OFF_SINE, note_hz(60 + t), GAIN, GAIN);
	submit_all(fd, ev, n, 20);
	start(fd, TEMPO_SLOW, 1);
	sleep_ms(1400);

	status(fd, &st);
	stalls = (1400000 - 20 * TEMPO_SLOW) / info.pi_tick_grain_us;
	check(st.pst_tick == 20, "the clock stops at the horizon (tick %u)",
	    st.pst_tick);
	check(st.pst_stalls + 5 >= stalls,
	    "counting the clock interrupts it waited (%u, about %u)",
	    st.pst_stalls, stalls);

	for (n = 0, t = 25; t <= 40; t += 5)
		ev[n++] = ev_tone(t, 0, OFF_SINE, note_hz(60 + t), GAIN, GAIN);
	ev[n++] = ev_stop(45, 0);
	submit_all(fd, ev, n, 45);
	check(wait_tick(fd, 45, 2000),
	    "and resumes once the horizon moves past it");
	close(fd);
}

static void
test_relatch(void)
{
	struct penaudio_event ev[64];
	struct penaudio_status st;
	uint32_t n = 0, t;
	int fd;

	printf("relatch: a loop change takes effect at the device's pass\n");
	fd = open_dev();
	load_samples(fd);
	if (alloc(fd, 2) < 2) {
		check(false, "two voices for the comparison");
		close(fd);
		return;
	}

	/*
	 * The same square, gated two ways: left by swapping its loop
	 * between the square and silence, right by starting and stopping
	 * it, which flushes and so is exact.  At 110 Hz the device's
	 * queue holds some 140 ms of the loop, so a loop change that
	 * waited for the queue would trail the right side by that much.
	 */
	for (t = 0; t < 150; t += 10) {
		bool on = (t / 10) % 2 == 0;

		if (t == 0) {
			ev[n++] = ev_tone(t, 0, OFF_SQUARE, 110, GAIN, 0);
			ev[n++] = ev_tone(t, 1, OFF_SQUARE, 110, 0, GAIN);
			continue;
		}
		ev[n++] = ev_loop(t, 0, on ? OFF_SQUARE : OFF_SILENCE);
		ev[n++] = on ? ev_tone(t, 1, OFF_SQUARE, 110, 0, GAIN) :
		    ev_stop(t, 1);
	}
	ev[n++] = ev_stop(150, 0);
	ev[n++] = ev_stop(150, 1);

	listen("a low buzz pulsing 2.5 times a second for 3 s, left and "
	    "right together; a left side trailing the right is the bug");
	submit_all(fd, ev, n, 150);
	start(fd, TEMPO_SLOW, 1);
	check(wait_tick(fd, 150, 5000), "the sequence runs to its end");
	status(fd, &st);
	check(st.pst_underruns == 0, "no underrun (%u)", st.pst_underruns);
	close(fd);
}

static void
test_pause(void)
{
	struct penaudio_event ev[16];
	struct penaudio_status st, st2;
	uint64_t t0, elapsed;
	uint32_t n = 0, t;
	int one = 1, zero = 0, fd;

	printf("pause: the clock and the voices freeze together\n");
	fd = open_dev();
	load_samples(fd);
	alloc(fd, 1);

	for (t = 0; t < 80; t += 10)
		ev[n++] = t == 0 ? ev_tone(0, 0, OFF_SINE, note_hz(60),
		    GAIN, GAIN) : ev_inc(t, 0, inc_for(note_hz(60 + t / 5),
		    CYCLE));
	ev[n++] = ev_stop(80, 0);

	listen("a rising run of eight notes that stops dead for 1 s "
	    "partway, then carries on from where it stopped");
	submit_all(fd, ev, n, 80);
	start(fd, TEMPO_SLOW, 1);
	t0 = now_us();
	sleep_ms(700);

	xioctl(fd, PENAUDIO_PAUSE, &one, "PAUSE");
	status(fd, &st);
	sleep_ms(1000);
	status(fd, &st2);
	check(st2.pst_tick == st.pst_tick && st2.pst_sounding == 1,
	    "a paused clock holds its tick (%u) and its voice", st.pst_tick);
	xioctl(fd, PENAUDIO_PAUSE, &zero, "PAUSE");

	check(wait_tick(fd, 80, 5000), "the sequence runs to its end");
	elapsed = now_us() - t0;
	check(elapsed > 2500000 && elapsed < 2800000,
	    "taking the 1.6 s of the tempo plus the 1 s paused (%llu ms)",
	    (unsigned long long)elapsed / 1000);
	close(fd);
}

static const struct test {
	const char	*name;
	void		(*fn)(void);
} tests[] = {
	{ "info",	test_info },
	{ "api",	test_api },
	{ "tone",	test_tone },
	{ "formats",	test_formats },
	{ "oneshot",	test_oneshot },
	{ "chord",	test_chord },
	{ "clock",	test_clock },
	{ "stall",	test_stall },
	{ "relatch",	test_relatch },
	{ "pause",	test_pause },
};

static void
usage(void)
{
	size_t i;

	fprintf(stderr, "usage: pmusic-test [test ...]\ntests:");
	for (i = 0; i < __arraycount(tests); i++)
		fprintf(stderr, " %s", tests[i].name);
	fprintf(stderr, "\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	size_t i;
	int fd, a;

	setvbuf(stdout, NULL, _IOLBF, 0);

	fd = open_dev();
	xioctl(fd, PENAUDIO_GETINFO, &info, "GETINFO");
	close(fd);

	if (argc == 1) {
		for (i = 0; i < __arraycount(tests); i++)
			tests[i].fn();
	} else {
		for (a = 1; a < argc; a++) {
			for (i = 0; i < __arraycount(tests); i++)
				if (strcmp(argv[a], tests[i].name) == 0)
					break;
			if (i == __arraycount(tests))
				usage();
			tests[i].fn();
		}
	}

	printf("%d failed\n", failures);
	return failures != 0;
}

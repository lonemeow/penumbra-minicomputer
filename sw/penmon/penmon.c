/*	$NetBSD$	*/
/*
 * penmon — Penumbra hardware-counter system monitor (main loop).
 *
 * Usage: penmon [-d interval_seconds]
 *
 * Each tick: snapshot all counters, derive per-interval rates, sample the
 * process table + memory, and repaint.  Keys: q quit, space force-refresh,
 * l or Ctrl-L redraw everything (the console is shared with kernel
 * messages, which land on top of the display), +/- change the interval.
 *
 * The display is driven by the screen layer (screen.h): a virtual-screen
 * damage diff that emits one write(2) per frame — no curses.
 */
#include "penmon.h"
#include "screen.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t want_quit;

static void
on_signal(int sig)
{
	(void)sig;
	want_quit = 1;
}

static void
sleep_ms(long ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int
main(int argc, char **argv)
{
	struct snapshot prev, cur;
	struct rates r;
	struct history hist;
	struct meminfo mem;
	struct procinfo procs[PENMON_MAXPROC];
	uint64_t clk_hz = 0;
	char cpu_model[PENMON_MODELLEN] = "";
	double interval = 1.0;
	int ch, nproc;
	double load[3];

	int c;
	while ((c = getopt(argc, argv, "d:h")) != -1) {
		switch (c) {
		case 'd':
			interval = atof(optarg);
			if (interval < 0.2) interval = 0.2;
			if (interval > 10.0) interval = 10.0;
			break;
		case 'h':
		default:
			fprintf(stderr, "usage: %s [-d interval]\n", argv[0]);
			return (c == 'h') ? 0 : 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	(void)read_cpu_freq(&clk_hz);
	if (!read_cpu_model(cpu_model, sizeof(cpu_model)))
		strlcpy(cpu_model, "Unknown CPU", sizeof(cpu_model));
	history_init(&hist);

	/* Prime with a short first interval so the screen isn't blank. */
	read_snapshot(&prev);
	sleep_ms(250);

	if (scr_init() < 0) {
		fprintf(stderr, "penmon: cannot initialise terminal\n");
		return 1;
	}

	for (;;) {
		scr_resize();		/* adopt a new terminal size if it changed */

		read_snapshot(&cur);
		compute_rates(&prev, &cur, clk_hz, &r);
		history_push(&hist, &r);
		read_meminfo(&mem);
		nproc = read_procs(procs, PENMON_MAXPROC);
		if (getloadavg(load, 3) < 1)
			load[0] = 0.0;

		render_frame(&r, &hist, &mem, load[0], read_uptime(),
		    procs, nproc, interval, cpu_model);

		prev = cur;

		ch = scr_getkey((int)(interval * 1000));	/* blocks up to interval */
		if (want_quit)
			break;
		switch (ch) {
		case 'q':
		case 'Q':
			goto done;
		case ' ':			/* force an immediate refresh */
			break;
		case '\f':			/* Ctrl-L: repair a damaged screen */
		case 'l':
		case 'L':
			scr_repaint();
			break;
		case '+':
		case '=':
			if (interval > 0.4) interval -= 0.2;
			break;
		case '-':
		case '_':
			if (interval < 10.0) interval += 0.2;
			break;
		default:
			break;			/* -1 (timeout) included */
		}
	}
done:
	scr_shutdown();
	return 0;
}

/*	$NetBSD$	*/
/*
 * penmon curses renderer.
 *
 * Fixed-layout dashboard.  Borders and bars use terminfo ACS line/block
 * glyphs (reliable over serial without a UTF-8 locale); sparklines use a
 * small ASCII intensity ramp.  Colour is threshold-based: green = good,
 * yellow = warning, red = bad — see metric_color().
 */
#include "penmon.h"

#include <curses.h>
#include <stdio.h>
#include <string.h>

/* Colour-pair ids. */
enum {
	PAIR_GREEN = 1,
	PAIR_YELLOW,
	PAIR_RED,
	PAIR_CYAN,
	PAIR_BLUE,
	PAIR_HDR,	/* title / table header */
	PAIR_DIM,	/* bar "track" / empty cells */
};

/* 8-level intensity ramp for sparklines, low -> high. */
static const char ramp[] = " .:-=+*#";
#define RAMP_LEVELS ((int)sizeof(ramp) - 1)

void
render_init(void)
{
	start_color();
	use_default_colors();		/* lets pair bg -1 mean "terminal default" */
	init_pair(PAIR_GREEN,  COLOR_GREEN,   -1);
	init_pair(PAIR_YELLOW, COLOR_YELLOW,  -1);
	init_pair(PAIR_RED,    COLOR_RED,     -1);
	init_pair(PAIR_CYAN,   COLOR_CYAN,    -1);
	init_pair(PAIR_BLUE,   COLOR_BLUE,    -1);
	init_pair(PAIR_HDR,    COLOR_WHITE,   COLOR_BLUE);
	init_pair(PAIR_DIM,    COLOR_BLACK,   -1);	/* +A_BOLD = grey */
}

/*
 * metric_color — map a value to a threshold colour pair.
 *
 * higher_is_better=1: v>=good is green, v>=warn is yellow, else red
 *                     (cache hit rate, MIPS, idle%).
 * higher_is_better=0: v<=good is green, v<=warn is yellow, else red
 *                     (CPI, miss rate, busy%).
 */
static int
metric_color(double v, double good, double warn, int higher_is_better)
{
	if (higher_is_better) {
		if (v >= good) return PAIR_GREEN;
		if (v >= warn) return PAIR_YELLOW;
		return PAIR_RED;
	}
	if (v <= good) return PAIR_GREEN;
	if (v <= warn) return PAIR_YELLOW;
	return PAIR_RED;
}

/*
 * cpi_quality — interpret a cycles-per-instruction value for the gauge.
 *
 * The "good" CPI is a property of the microarchitecture, which changes
 * across generations (gen1's microcoded core floors around 3 CPI; a
 * future pipelined gen2 aims for ~1), so there is no fixed scale.  Instead
 * we calibrate against this machine's own best-observed CPI: the gauge
 * shows headroom above that floor — empty = running at its best, filling =
 * stalling more than usual right now.  Generation-proof, no constants.
 */
static void
cpi_quality(double cpi, double *frac, int *pair)
{
	/*
	 * The realistic CPI band on a given microarchitecture is narrow
	 * (gen1 sits ~3.7 and only climbs past ~5 under pathological uncached
	 * / MMIO access), so the gauge is scaled to a small headroom above the
	 * discovered floor — otherwise the bar would never visibly move.
	 * FULL_SCALE is the one knob: CPI of floor*FULL_SCALE fills the bar.
	 * Retune per generation if the observed spread changes.
	 */
	static const double FULL_SCALE = 1.4;	/* full bar at 1.4x best CPI */
	static double floor;			/* lowest CPI seen this session */
	double span, f;

	if (cpi > 0.0 && (floor == 0.0 || cpi < floor))
		floor = cpi;		/* converges to the architectural floor */

	span = floor * (FULL_SCALE - 1.0);
	f = span > 0.0 ? (cpi - floor) / span : 0.0;
	if (f < 0.0) f = 0.0;
	if (f > 1.0) f = 1.0;
	*frac = f;

	if (cpi < floor * 1.10)
		*pair = PAIR_GREEN;	/* within 10% of best */
	else if (cpi < floor * 1.25)
		*pair = PAIR_YELLOW;	/* 10-25% over best */
	else
		*pair = PAIR_RED;	/* >25% over best — stall-bound */
}

void
history_init(struct history *h)
{
	memset(h, 0, sizeof(*h));
}

void
history_push(struct history *h, const struct rates *r)
{
	h->head = (h->head + 1) % HIST_LEN;
	h->cpi[h->head] = (float)r->cpi;
	h->l1i[h->head] = (float)r->l1i.hit_pct;
	h->l1d[h->head] = (float)r->l1d.hit_pct;
	h->l2[h->head]  = (float)r->l2.hit_pct;
	if (h->count < HIST_LEN)
		h->count++;
}

/* Draw a w-cell bar at (y,x) filled to `frac` (0..1) in colour `pair`,
 * with a dim checkerboard track behind the empty part. */
static void
draw_bar(int y, int x, int w, double frac, int pair)
{
	int i, fill;

	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	fill = (int)(frac * w + 0.5);

	for (i = 0; i < w; i++) {
		if (i < fill)
			mvaddch(y, x + i,
			    ACS_BLOCK | COLOR_PAIR(pair) | A_BOLD);
		else
			mvaddch(y, x + i,
			    ACS_CKBOARD | COLOR_PAIR(PAIR_DIM) | A_BOLD);
	}
}

/* Stacked CPU bar: user|sys|intr fill, idle is the track. */
static void
draw_cpu_bar(int y, int x, int w, const double pct[5])
{
	double busy = pct[CP_USER] + pct[CP_NICE] + pct[CP_SYS] +
	    pct[CP_INTR];
	int wu, ws, wi, used, i;

	wu = (int)((pct[CP_USER] + pct[CP_NICE]) / 100.0 * w + 0.5);
	ws = (int)(pct[CP_SYS]  / 100.0 * w + 0.5);
	wi = (int)(pct[CP_INTR] / 100.0 * w + 0.5);
	used = 0;

	for (i = 0; i < wu && used < w; i++, used++)
		mvaddch(y, x + used, ACS_BLOCK | COLOR_PAIR(PAIR_GREEN) | A_BOLD);
	for (i = 0; i < ws && used < w; i++, used++)
		mvaddch(y, x + used, ACS_BLOCK | COLOR_PAIR(PAIR_CYAN) | A_BOLD);
	for (i = 0; i < wi && used < w; i++, used++)
		mvaddch(y, x + used, ACS_BLOCK | COLOR_PAIR(PAIR_RED) | A_BOLD);
	for (; used < w; used++)
		mvaddch(y, x + used, ACS_CKBOARD | COLOR_PAIR(PAIR_DIM) | A_BOLD);

	(void)busy;
}

/* Sparkline of the last `w` samples from a history ring. */
static void
draw_spark(int y, int x, int w, const float *ring, int count, int head,
    double vmax, double good, double warn, int higher_is_better)
{
	int i;

	for (i = 0; i < w; i++) {
		int age = w - 1 - i;		/* 0 = newest at right edge */
		int idx, lvl, pair;
		float v;

		if (age >= count) {		/* no sample yet */
			mvaddch(y, x + i, ' ');
			continue;
		}
		idx = (head - age) % HIST_LEN;
		if (idx < 0)
			idx += HIST_LEN;
		v = ring[idx];

		lvl = vmax > 0 ? (int)((double)v / vmax * (RAMP_LEVELS - 1) + 0.5) : 0;
		if (lvl < 0) lvl = 0;
		if (lvl >= RAMP_LEVELS) lvl = RAMP_LEVELS - 1;
		pair = metric_color(v, good, warn, higher_is_better);

		mvaddch(y, x + i,
		    (chtype)ramp[lvl] | COLOR_PAIR(pair) | A_BOLD);
	}
}

/* Format a byte count compactly into buf (e.g. "12.3M"). */
static void
human_bytes(uint64_t b, char *buf, size_t bufsz)
{
	if (b >= 1024ULL * 1024 * 1024)
		snprintf(buf, bufsz, "%.1fG", (double)b / (1024*1024*1024));
	else if (b >= 1024ULL * 1024)
		snprintf(buf, bufsz, "%.1fM", (double)b / (1024*1024));
	else if (b >= 1024)
		snprintf(buf, bufsz, "%.0fK", (double)b / 1024);
	else
		snprintf(buf, bufsz, "%lluB", (unsigned long long)b);
}

/* One cache row: label, hit%, hit bar, miss/s, sparkline. */
static void
cache_row(int y, const char *label, const struct cache_rate *cr,
    const float *ring, int count, int head, int spark_w)
{
	int c = metric_color(cr->hit_pct, 90.0, 70.0, 1);

	mvprintw(y, 1, "%-4s", label);
	attron(COLOR_PAIR(c) | A_BOLD);
	mvprintw(y, 6, "%5.1f%%", cr->hit_pct);
	attroff(COLOR_PAIR(c) | A_BOLD);
	draw_bar(y, 13, 20, cr->hit_pct / 100.0, c);
	mvprintw(y, 35, "%7.0f/s", cr->miss_per_sec);
	/* hit% sparkline: scale 0..100, good>=90 warn>=70 */
	draw_spark(y, 47, spark_w, ring, count, head, 100.0, 90.0, 70.0, 1);
}

void
render_frame(const struct rates *r, const struct history *h,
    const struct meminfo *mem, double load1, long uptime_sec,
    const struct procinfo *procs, int nproc, double interval)
{
	int y, i, cpi_c, spark_w;
	char lbuf[16], rbuf[16];
	long up = uptime_sec;

	erase();
	spark_w = COLS - 48;
	if (spark_w < 8) spark_w = 8;
	if (spark_w > HIST_LEN) spark_w = HIST_LEN;

	/* ── Title bar ─────────────────────────────────────────── */
	attron(COLOR_PAIR(PAIR_HDR) | A_BOLD);
	for (i = 0; i < COLS; i++)
		mvaddch(0, i, ' ');
	mvprintw(0, 1, "PENUMBRA penmon");
	mvprintw(0, COLS - 38, "up %ld:%02ld:%02ld  load %.2f  %.1f MHz",
	    up / 3600, (up % 3600) / 60, up % 60, load1, r->clk_mhz);
	attroff(COLOR_PAIR(PAIR_HDR) | A_BOLD);

	/* ── CPU + CPI ─────────────────────────────────────────── */
	y = 2;
	mvprintw(y, 1, "CPU");
	draw_cpu_bar(y, 6, 28, r->cpu_pct);
	mvprintw(y, 36, "us%4.0f%% sy%4.0f%% in%4.0f%% id%4.0f%%",
	    r->cpu_pct[CP_USER] + r->cpu_pct[CP_NICE], r->cpu_pct[CP_SYS],
	    r->cpu_pct[CP_INTR], r->cpu_pct[CP_IDLE]);

	y = 3;
	{
		double cpi_frac = 0.0;

		cpi_c = PAIR_GREEN;
		cpi_quality(r->cpi, &cpi_frac, &cpi_c);
		mvprintw(y, 1, "CPI");
		attron(COLOR_PAIR(cpi_c) | A_BOLD);
		mvprintw(y, 6, "%5.2f", r->cpi);
		attroff(COLOR_PAIR(cpi_c) | A_BOLD);
		draw_bar(y, 13, 20, cpi_frac, cpi_c);
	}
	mvprintw(y, 36, "MIPS %6.2f", r->mips);

	y = 4;
	mvprintw(y, 1, "STALL");
	mvprintw(y, 7,
	    "funit%5.1f%%  ifetch%5.1f%%  load%5.1f%%  store%5.1f%%",
	    r->stall_funit_pct, r->stall_ifetch_pct,
	    r->stall_load_pct, r->stall_store_pct);

	mvhline(5, 0, ACS_HLINE, COLS);

	/* ── Cache panel ───────────────────────────────────────── */
	mvprintw(6, 1, "CACHE      hit%%        (hit bar)      miss/s  history");
	cache_row(7, "L1I", &r->l1i, h->l1i, h->count, h->head, spark_w);
	cache_row(8, "L1D", &r->l1d, h->l1d, h->count, h->head, spark_w);
	cache_row(9, "L2",  &r->l2,  h->l2,  h->count, h->head, spark_w);

	mvhline(10, 0, ACS_HLINE, COLS);

	/* ── Memory ────────────────────────────────────────────── */
	{
		double used_frac = mem->total_bytes ?
		    (double)(mem->total_bytes - mem->free_bytes) /
		    (double)mem->total_bytes : 0.0;
		int mc = metric_color(used_frac * 100.0, 75.0, 90.0, 0);
		char fbuf[16];

		human_bytes(mem->total_bytes - mem->free_bytes, lbuf, sizeof(lbuf));
		human_bytes(mem->total_bytes, rbuf, sizeof(rbuf));
		human_bytes(mem->free_bytes, fbuf, sizeof(fbuf));
		mvprintw(11, 1, "MEM");
		draw_bar(11, 6, 28, used_frac, mc);
		mvprintw(11, 36, "%s / %s used  (%s free)   flt %.0f/s",
		    lbuf, rbuf, fbuf, r->faults_per_sec);
	}

	/* ── Activity (vmstat-style rates from uvmexp2) ────────── */
	mvprintw(12, 1,
	    "ACT  intr %5.0f/s  syscall %6.0f/s  csw %5.0f/s  fork %4.0f/s",
	    r->intr_per_sec, r->syscall_per_sec, r->csw_per_sec,
	    r->fork_per_sec);

	mvhline(13, 0, ACS_HLINE, COLS);

	/* ── Process table ─────────────────────────────────────── */
	attron(COLOR_PAIR(PAIR_HDR) | A_BOLD);
	for (i = 0; i < COLS; i++)
		mvaddch(14, i, ' ');
	mvprintw(14, 1, "%6s %-10s %5s %8s %2s %s",
	    "PID", "USER", "%CPU", "RSS", "ST", "COMMAND");
	attroff(COLOR_PAIR(PAIR_HDR) | A_BOLD);

	for (i = 0; i < nproc && (15 + i) < LINES - 1; i++) {
		const struct procinfo *p = &procs[i];
		int pc = metric_color(p->pctcpu, 1.0, 20.0, 1);

		human_bytes(p->rss_bytes, rbuf, sizeof(rbuf));
		mvprintw(15 + i, 1, "%6d %-10.10s ", p->pid, p->user);
		attron(COLOR_PAIR(pc) | A_BOLD);
		mvprintw(15 + i, 19, "%5.1f", p->pctcpu);
		attroff(COLOR_PAIR(pc) | A_BOLD);
		mvprintw(15 + i, 25, " %8s %c  %-.*s",
		    rbuf, p->state, COLS - 40, p->comm);
	}

	/* ── Help line ─────────────────────────────────────────── */
	attron(COLOR_PAIR(PAIR_DIM) | A_BOLD);
	mvprintw(LINES - 1, 1,
	    "q quit   space refresh   +/- interval (%.1fs)", interval);
	attroff(COLOR_PAIR(PAIR_DIM) | A_BOLD);

	refresh();
}

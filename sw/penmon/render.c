/*	$NetBSD$	*/
/*
 * penmon renderer — fixed-layout dashboard drawn into the screen layer.
 *
 * Borders and bars use Unicode box/block glyphs (emitted as raw UTF-8 by
 * screen.c — no curses, no locale dependency); sparklines use a Unicode
 * block-height ramp.  Colour is threshold-based: green = good, yellow =
 * warning, red = bad — see metric_color().
 *
 * The renderer redraws the whole logical frame every tick; the screen
 * layer's damage diff turns that into only-the-changed-cells on the wire.
 */
#include "penmon.h"
#include "screen.h"

#include <stdio.h>
#include <string.h>

/*
 * Colour-pair ids.  Kept as an indirection so the threshold helpers stay
 * colour-agnostic; pair_attr() maps a pair to a screen attribute.
 */
enum {
	PAIR_GREEN = 1,
	PAIR_YELLOW,
	PAIR_RED,
	PAIR_CYAN,
	PAIR_BLUE,
	PAIR_HDR,	/* title / table header: white on blue */
	PAIR_DIM,	/* bar track / empty cells: grey        */
};

#define A_NORM ATTR(C_DEFAULT, C_DEFAULT, 0)

static scr_attr
pair_attr(int pair)
{
	switch (pair) {
	case PAIR_GREEN:  return ATTR(C_GREEN,  C_DEFAULT, 1);
	case PAIR_YELLOW: return ATTR(C_YELLOW, C_DEFAULT, 1);
	case PAIR_RED:    return ATTR(C_RED,    C_DEFAULT, 1);
	case PAIR_CYAN:   return ATTR(C_CYAN,   C_DEFAULT, 1);
	case PAIR_BLUE:   return ATTR(C_BLUE,   C_DEFAULT, 1);
	case PAIR_HDR:    return ATTR(C_WHITE,  C_BLUE,    1);
	case PAIR_DIM:    return ATTR(C_BLACK,  C_DEFAULT, 1);	/* grey */
	default:          return A_NORM;
	}
}

/* 8-level block-height ramp for sparklines, low -> high (U+2581..U+2588). */
static const uint32_t ramp[] = {
	0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588
};
#define RAMP_LEVELS ((int)(sizeof(ramp) / sizeof(ramp[0])))

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
 * with a dim shaded track behind the empty part. */
static void
draw_bar(int y, int x, int w, double frac, int pair)
{
	int i, fill;

	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	fill = (int)(frac * w + 0.5);

	for (i = 0; i < w; i++) {
		if (i < fill)
			scr_putc(y, x + i, GLYPH_BLOCK, pair_attr(pair));
		else
			scr_putc(y, x + i, GLYPH_TRACK, pair_attr(PAIR_DIM));
	}
}

/* Stacked CPU bar: user|sys|intr fill, idle is the track. */
static void
draw_cpu_bar(int y, int x, int w, const double pct[5])
{
	int wu, ws, wi, used, i;

	wu = (int)((pct[CP_USER] + pct[CP_NICE]) / 100.0 * w + 0.5);
	ws = (int)(pct[CP_SYS]  / 100.0 * w + 0.5);
	wi = (int)(pct[CP_INTR] / 100.0 * w + 0.5);
	used = 0;

	for (i = 0; i < wu && used < w; i++, used++)
		scr_putc(y, x + used, GLYPH_BLOCK, pair_attr(PAIR_GREEN));
	for (i = 0; i < ws && used < w; i++, used++)
		scr_putc(y, x + used, GLYPH_BLOCK, pair_attr(PAIR_CYAN));
	for (i = 0; i < wi && used < w; i++, used++)
		scr_putc(y, x + used, GLYPH_BLOCK, pair_attr(PAIR_RED));
	for (; used < w; used++)
		scr_putc(y, x + used, GLYPH_TRACK, pair_attr(PAIR_DIM));
}

/*
 * Sparkline of the last `w` samples from a history ring.  The whole strip
 * takes a single colour — chosen from the most recent sample — so it costs
 * one SGR run on the wire instead of one per cell; the glyph heights still
 * carry the history, the colour reads as "current state".
 */
static void
draw_spark(int y, int x, int w, const float *ring, int count, int head,
    double vmax, double good, double warn, int higher_is_better)
{
	scr_attr a;
	int i;

	a = pair_attr(metric_color(count > 0 ? ring[head] : 0.0,
	    good, warn, higher_is_better));

	for (i = 0; i < w; i++) {
		int age = w - 1 - i;		/* 0 = newest at right edge */
		int idx, lvl;
		float v;

		if (age >= count) {		/* no sample yet */
			scr_putc(y, x + i, ' ', A_NORM);
			continue;
		}
		idx = (head - age) % HIST_LEN;
		if (idx < 0)
			idx += HIST_LEN;
		v = ring[idx];

		lvl = vmax > 0 ? (int)((double)v / vmax * (RAMP_LEVELS - 1) + 0.5) : 0;
		if (lvl < 0) lvl = 0;
		if (lvl >= RAMP_LEVELS) lvl = RAMP_LEVELS - 1;

		scr_putc(y, x + i, ramp[lvl], a);
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

	scr_printf(y, 1, A_NORM, "%-4s", label);
	scr_printf(y, 6, pair_attr(c), "%5.1f%%", cr->hit_pct);
	draw_bar(y, 13, 20, cr->hit_pct / 100.0, c);
	scr_printf(y, 35, A_NORM, "%7.0f/s", cr->miss_per_sec);
	/* hit% sparkline: scale 0..100, good>=90 warn>=70 */
	draw_spark(y, 47, spark_w, ring, count, head, 100.0, 90.0, 70.0, 1);
}

void
render_frame(const struct rates *r, const struct history *h,
    const struct meminfo *mem, double load1, long uptime_sec,
    const struct procinfo *procs, int nproc, double interval)
{
	int y, i, cpi_c, spark_w, cols, lines;
	char lbuf[16], rbuf[16];
	long up = uptime_sec;

	scr_clear();
	cols = scr_cols();
	lines = scr_rows();
	spark_w = cols - 48;
	if (spark_w < 8) spark_w = 8;
	if (spark_w > HIST_LEN) spark_w = HIST_LEN;

	/* ── Title bar ─────────────────────────────────────────── */
	scr_fill(0, 0, cols, ' ', pair_attr(PAIR_HDR));
	scr_printf(0, 1, pair_attr(PAIR_HDR), "PENUMBRA penmon");
	scr_printf(0, cols - 38, pair_attr(PAIR_HDR),
	    "up %ld:%02ld:%02ld  load %.2f  %.1f MHz",
	    up / 3600, (up % 3600) / 60, up % 60, load1, r->clk_mhz);

	/* ── CPU + CPI ─────────────────────────────────────────── */
	y = 2;
	scr_printf(y, 1, A_NORM, "CPU");
	draw_cpu_bar(y, 6, 28, r->cpu_pct);
	scr_printf(y, 36, A_NORM, "us%4.0f%% sy%4.0f%% in%4.0f%% id%4.0f%%",
	    r->cpu_pct[CP_USER] + r->cpu_pct[CP_NICE], r->cpu_pct[CP_SYS],
	    r->cpu_pct[CP_INTR], r->cpu_pct[CP_IDLE]);

	y = 3;
	{
		double cpi_frac = 0.0;

		cpi_c = PAIR_GREEN;
		cpi_quality(r->cpi, &cpi_frac, &cpi_c);
		scr_printf(y, 1, A_NORM, "CPI");
		scr_printf(y, 6, pair_attr(cpi_c), "%5.2f", r->cpi);
		draw_bar(y, 13, 20, cpi_frac, cpi_c);
	}
	scr_printf(y, 36, A_NORM, "MIPS %6.2f", r->mips);

	y = 4;
	scr_printf(y, 1, A_NORM, "STALL");
	scr_printf(y, 7, A_NORM,
	    "funit%5.1f%%  ifetch%5.1f%%  load%5.1f%%  store%5.1f%%",
	    r->stall_funit_pct, r->stall_ifetch_pct,
	    r->stall_load_pct, r->stall_store_pct);

	scr_fill(5, 0, cols, GLYPH_HLINE, A_NORM);

	/* ── Cache panel ───────────────────────────────────────── */
	scr_printf(6, 1, A_NORM,
	    "CACHE      hit%%        (hit bar)      miss/s  history");
	cache_row(7, "L1I", &r->l1i, h->l1i, h->count, h->head, spark_w);
	cache_row(8, "L1D", &r->l1d, h->l1d, h->count, h->head, spark_w);
	cache_row(9, "L2",  &r->l2,  h->l2,  h->count, h->head, spark_w);

	scr_fill(10, 0, cols, GLYPH_HLINE, A_NORM);

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
		scr_printf(11, 1, A_NORM, "MEM");
		draw_bar(11, 6, 28, used_frac, mc);
		scr_printf(11, 36, A_NORM, "%s / %s used  (%s free)   flt %.0f/s",
		    lbuf, rbuf, fbuf, r->faults_per_sec);
	}

	/* ── Activity (vmstat-style rates from uvmexp2) ────────── */
	scr_printf(12, 1, A_NORM,
	    "ACT  intr %5.0f/s  syscall %6.0f/s  csw %5.0f/s  fork %4.0f/s",
	    r->intr_per_sec, r->syscall_per_sec, r->csw_per_sec,
	    r->fork_per_sec);

	scr_fill(13, 0, cols, GLYPH_HLINE, A_NORM);

	/* ── Process table ─────────────────────────────────────── */
	scr_fill(14, 0, cols, ' ', pair_attr(PAIR_HDR));
	scr_printf(14, 1, pair_attr(PAIR_HDR), "%6s %-10s %5s %8s %2s %s",
	    "PID", "USER", "%CPU", "RSS", "ST", "COMMAND");

	for (i = 0; i < nproc && (15 + i) < lines - 1; i++) {
		const struct procinfo *p = &procs[i];
		int pc = metric_color(p->pctcpu, 1.0, 20.0, 1);

		human_bytes(p->rss_bytes, rbuf, sizeof(rbuf));
		scr_printf(15 + i, 1, A_NORM, "%6d %-10.10s ", p->pid, p->user);
		scr_printf(15 + i, 19, pair_attr(pc), "%5.1f", p->pctcpu);
		scr_printf(15 + i, 25, A_NORM, " %8s %c  %-.*s",
		    rbuf, p->state, cols - 40, p->comm);
	}

	/* ── Help line ─────────────────────────────────────────── */
	scr_printf(lines - 1, 1, pair_attr(PAIR_DIM),
	    "q quit   space refresh   +/- interval (%.1fs)", interval);

	scr_flush();
}

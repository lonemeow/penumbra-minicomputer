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
	PAIR_MAGENTA,
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
	case PAIR_CYAN:   return ATTR(C_CYAN,    C_DEFAULT, 1);
	case PAIR_BLUE:   return ATTR(C_BLUE,    C_DEFAULT, 1);
	case PAIR_MAGENTA:return ATTR(C_MAGENTA, C_DEFAULT, 1);
	case PAIR_HDR:    return ATTR(C_WHITE,   C_BLUE,    1);
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

/*
 * Draw a stacked proportional bar: segment i is sized to pct[i] percent of
 * the bar width and drawn in colour pairs[i]; the unfilled remainder is the
 * dim track.  Segments are clipped to the bar, so percentages that sum past
 * 100 just fill it rather than overrun.
 */
static void
draw_stack_bar(int y, int x, int w, const double *pct, const int *pairs, int n)
{
	int used = 0, i, j;

	for (i = 0; i < n; i++) {
		int seg = (int)(pct[i] / 100.0 * w + 0.5);

		for (j = 0; j < seg && used < w; j++, used++)
			scr_putc(y, x + used, GLYPH_BLOCK, pair_attr(pairs[i]));
	}
	for (; used < w; used++)
		scr_putc(y, x + used, GLYPH_TRACK, pair_attr(PAIR_DIM));
}

/* Stacked CPU bar: user+nice | sys | intr fill, idle is the track. */
static void
draw_cpu_bar(int y, int x, int w, const double pct[5])
{
	const double seg[] = {
		pct[CP_USER] + pct[CP_NICE], pct[CP_SYS], pct[CP_INTR]
	};
	static const int pairs[] = { PAIR_GREEN, PAIR_CYAN, PAIR_RED };

	draw_stack_bar(y, x, w, seg, pairs, (int)(sizeof(seg) / sizeof(seg[0])));
}

/*
 * Stacked STALL bar: the six per-cause stall percentages fill the bar in
 * the same left-to-right order as the STALL readout above; the productive
 * (non-stalled) remainder is the track, so the filled length reads as the
 * total stall burden and the colours break it down by cause.
 */
static void
draw_stall_bar(int y, int x, int w, const struct rates *r)
{
	const double seg[] = {
		r->stall_funit_pct, r->stall_ifetch_pct, r->stall_load_pct,
		r->stall_store_pct, r->stall_hazard_pct, r->stall_flush_pct
	};
	/*
	 * Colours reuse the dashboard's good/warn/bad reading for the causes
	 * that carry one: funit (multi-cycle execution the core genuinely has
	 * to do) is green, ifetch (the caches failing to feed the front end)
	 * is red, flush (branch mispredict / redirect) is yellow.  load, store,
	 * and hazard have no inherent good-or-bad sense, so they take neutral
	 * hues (cyan, magenta, blue) picked only to keep the segments distinct.
	 */
	static const int pairs[] = {
		PAIR_GREEN, PAIR_RED, PAIR_CYAN, PAIR_MAGENTA, PAIR_BLUE, PAIR_YELLOW
	};

	draw_stack_bar(y, x, w, seg, pairs, (int)(sizeof(seg) / sizeof(seg[0])));
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

/*
 * Responsive column layout.  Every gauge row is a three-zone grid: a
 * fixed left zone (label + inline value) up to BAR_X, a stretchable bar
 * zone that grows with the terminal, and a fixed-width right "info" zone
 * (trailing readouts; for cache rows, miss/s + the history sparkline).
 * One layout drives every row so the bars line up as a single column.
 */
#define BAR_X      13	/* left edge shared by the CPU/CPI/cache/MEM bars */
#define INFO_W     48	/* min cols reserved for readouts (MEM is widest) */

/* Per-source interrupt panel: shown only when the terminal has room to
 * spare beyond the fixed panels, the process header, and a few process
 * rows worth keeping. */
#define IRQ_MIN_LINES  26	/* fixed panels + header + rows worth keeping */
#define IRQ_MIN_COLS   72	/* two sources per line, side by side */
#define IRQ_MAX_SHOWN  8
#define IRQ_LABEL_W    22
#define BAR_GAP     2	/* blank columns between a bar and the info zone  */
#define MIN_BAR_W  12	/* floor so a narrow terminal never collapses it  */
#define MISS_W      9	/* "%7.0f/s" — the cache miss-rate field width    */
#define SPARK_GAP   1	/* blank column between miss/s and the sparkline  */

struct layout {
	int bar_x;	/* left edge of every stretchable bar */
	int bar_w;	/* their (terminal-dependent) width   */
	int info_x;	/* first column of the right info zone */
	int spark_x;	/* first column of the cache sparkline */
	int spark_w;	/* sparkline width                     */
};

/*
 * Derive the per-frame column geometry from the current terminal width.
 * bar_x and INFO_W are fixed; everything else stretches with `cols`.
 */
static void
compute_layout(int cols, struct layout *L)
{
	int avail, bar_max;

	memset(L, 0, sizeof(*L));
	L->bar_x = BAR_X;

	/*
	 * The bar zone and the cache history sparkline are the two parts that
	 * stretch; the label/value, miss/s field, and gaps are fixed.  Split
	 * the stretchable span 2:1 in the bars' favour — the gauge is the
	 * story, the history is context.
	 */
	avail = cols - BAR_X - BAR_GAP - MISS_W - SPARK_GAP;
	L->bar_w = avail * 2 / 3;

	/*
	 * Never let the bar grow so wide that the widest trailing readout (the
	 * MEM line, INFO_W cols) loses room: on a narrow terminal the bar
	 * gives ground rather than truncating the numbers.  MIN_BAR_W keeps it
	 * from vanishing entirely below ~70 cols.
	 */
	bar_max = cols - BAR_X - BAR_GAP - INFO_W;
	if (L->bar_w > bar_max)
		L->bar_w = bar_max;
	if (L->bar_w < MIN_BAR_W)
		L->bar_w = MIN_BAR_W;

	L->info_x  = BAR_X + L->bar_w + BAR_GAP;
	L->spark_x = L->info_x + MISS_W + SPARK_GAP;

	/*
	 * Hold the 2:1 ratio from the history side too: the sparkline fills
	 * what's left of the line but is never wider than half the bar, so on
	 * a narrow terminal it shrinks (leaving right-margin blank) instead of
	 * dwarfing a bar that the MEM readout has already squeezed.
	 */
	L->spark_w = cols - L->spark_x;
	if (L->spark_w > L->bar_w / 2)
		L->spark_w = L->bar_w / 2;
	if (L->spark_w > HIST_LEN)
		L->spark_w = HIST_LEN;
	if (L->spark_w < 1)
		L->spark_w = 1;
}

/* One cache row: label, hit%, hit bar, miss/s, sparkline. */
static void
cache_row(int y, const char *label, const struct cache_rate *cr,
    const float *ring, int count, int head, const struct layout *L)
{
	int c = metric_color(cr->hit_pct, 90.0, 70.0, 1);

	scr_printf(y, 1, A_NORM, "%-4s", label);
	scr_printf(y, 6, pair_attr(c), "%5.1f%%", cr->hit_pct);
	draw_bar(y, L->bar_x, L->bar_w, cr->hit_pct / 100.0, c);
	scr_printf(y, L->info_x, A_NORM, "%7.0f/s", cr->miss_per_sec);
	/* hit% sparkline: scale 0..100, good>=90 warn>=70 */
	draw_spark(y, L->spark_x, L->spark_w, ring, count, head,
	    100.0, 90.0, 70.0, 1);
}

void
render_frame(const struct rates *r, const struct history *h,
    const struct meminfo *mem, double load1, long uptime_sec,
    const struct procinfo *procs, int nproc, double interval,
    const char *cpu_model)
{
	int y, i, cpi_c, cols, lines;
	char lbuf[16], rbuf[16];
	long up = uptime_sec;
	struct layout lay;

	scr_clear();
	cols = scr_cols();
	lines = scr_rows();
	compute_layout(cols, &lay);

	/* ── Title bar ─────────────────────────────────────────── */
	scr_fill(0, 0, cols, ' ', pair_attr(PAIR_HDR));
	scr_printf(0, 1, pair_attr(PAIR_HDR), "%s @ %.1f MHz",
		cpu_model, r->clk_mhz);
	scr_printf(0, cols - 27, pair_attr(PAIR_HDR),
	    "up %02ld:%02ld:%02ld  load avg %.2f",
	    up / 3600, (up % 3600) / 60, up % 60, load1);

	/* ── CPU + CPI ─────────────────────────────────────────── */
	y = 2;
	scr_printf(y, 1, A_NORM, "CPU");
	draw_cpu_bar(y, lay.bar_x, lay.bar_w, r->cpu_pct);
	scr_printf(y, lay.info_x, A_NORM, "us%4.0f%% sy%4.0f%% in%4.0f%% id%4.0f%%",
	    r->cpu_pct[CP_USER] + r->cpu_pct[CP_NICE], r->cpu_pct[CP_SYS],
	    r->cpu_pct[CP_INTR], r->cpu_pct[CP_IDLE]);

	y = 3;
	{
		double cpi_frac = 0.0;

		cpi_c = PAIR_GREEN;
		cpi_quality(r->cpi, &cpi_frac, &cpi_c);
		scr_printf(y, 1, A_NORM, "CPI");
		scr_printf(y, 6, pair_attr(cpi_c), "%5.2f", r->cpi);
		draw_bar(y, lay.bar_x, lay.bar_w, cpi_frac, cpi_c);
	}
	scr_printf(y, lay.info_x, A_NORM, "MIPS %6.2f", r->mips);

	y = 4;
	scr_printf(y, 1, A_NORM, "STALL");
	scr_printf(y, 7, A_NORM,
	    "funit%5.1f%% ifetch%5.1f%% load%5.1f%% store%5.1f%% hazard%5.1f%% flush%5.1f%%",
	    r->stall_funit_pct, r->stall_ifetch_pct,
	    r->stall_load_pct, r->stall_store_pct,
	    r->stall_hazard_pct, r->stall_flush_pct);
	draw_stall_bar(5, lay.bar_x, cols - lay.bar_x - 1, r);

	scr_fill(6, 0, cols, GLYPH_HLINE, A_NORM);

	/* ── Cache panel ───────────────────────────────────────── */
	scr_printf(7, 1, A_NORM, "CACHE");
	scr_printf(7, 6, A_NORM, " hit%%");
	scr_printf(7, lay.bar_x, A_NORM, "(hit rate)");
	scr_printf(7, lay.info_x, A_NORM, "miss/s");
	scr_printf(7, lay.spark_x, A_NORM, "history");
	cache_row(8, "L1I", &r->l1i, h->l1i, h->count, h->head, &lay);
	cache_row(9, "L1D", &r->l1d, h->l1d, h->count, h->head, &lay);
	cache_row(10, "L2",  &r->l2,  h->l2,  h->count, h->head, &lay);

	scr_fill(11, 0, cols, GLYPH_HLINE, A_NORM);

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
		scr_printf(12, 1, A_NORM, "MEM");
		draw_bar(12, lay.bar_x, lay.bar_w, used_frac, mc);
		scr_printf(12, lay.info_x, A_NORM, "%s / %s used  (%s free)   flt %.0f/s",
		    lbuf, rbuf, fbuf, r->faults_per_sec);
	}

	/* ── Activity (vmstat-style rates from uvmexp2) ────────── */
	scr_printf(13, 1, A_NORM,
	    "ACT  intr %5.0f/s  syscall %6.0f/s  csw %5.0f/s  fork %4.0f/s",
	    r->intr_per_sec, r->syscall_per_sec, r->csw_per_sec,
	    r->fork_per_sec);

	scr_fill(14, 0, cols, GLYPH_HLINE, A_NORM);
	y = 15;

	/* ── Per-source interrupts ─────────────────────────────────
	 * Only on a terminal with rows to spare: the process table is
	 * the panel a small screen must keep, so this one appears when
	 * both fit.  Two sources per line, busiest first. */
	if (r->nirq > 0 && cols >= IRQ_MIN_COLS &&
	    lines >= IRQ_MIN_LINES + (r->nirq + 1) / 2) {
		int n = r->nirq > IRQ_MAX_SHOWN ? IRQ_MAX_SHOWN : r->nirq;

		scr_printf(y, 1, pair_attr(PAIR_HDR), "IRQ");
		for (i = 0; i < n; i++) {
			int col = (i & 1) ? cols / 2 : 6;

			scr_printf(y + i / 2, col, A_NORM, "%-*.*s %7.0f/s",
			    IRQ_LABEL_W, IRQ_LABEL_W, r->irq[i].name,
			    r->irq[i].per_sec);
		}
		y += (n + 1) / 2;
		scr_fill(y, 0, cols, GLYPH_HLINE, A_NORM);
		y++;
	}

	/* ── Process table ─────────────────────────────────────── */
	scr_fill(y, 0, cols, ' ', pair_attr(PAIR_HDR));
	scr_printf(y, 1, pair_attr(PAIR_HDR), "%6s %-10s %5s %8s %2s %s",
	    "PID", "USER", "%CPU", "RSS", "ST", "COMMAND");
	y++;

	for (i = 0; i < nproc && (y + i) < lines - 1; i++) {
		const struct procinfo *p = &procs[i];
		int pc = metric_color(p->pctcpu, 1.0, 20.0, 1);

		human_bytes(p->rss_bytes, rbuf, sizeof(rbuf));
		scr_printf(y + i, 1, A_NORM, "%6d %-10.10s ", p->pid, p->user);
		scr_printf(y + i, 19, pair_attr(pc), "%5.1f", p->pctcpu);
		scr_printf(y + i, 25, A_NORM, " %8s %c  %-.*s",
		    rbuf, p->state, cols - 40, p->comm);
	}

	/* ── Help line ─────────────────────────────────────────── */
	scr_printf(lines - 1, 1, pair_attr(PAIR_DIM),
	    "q quit   space refresh   l redraw   +/- interval (%.1fs)", interval);

	scr_flush();
}

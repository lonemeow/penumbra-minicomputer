/* life1d.c — one-dimensional Life, the program Usagi Electric runs on
 * vintage machines.
 *
 * A row of cells evolves a generation at a time and each generation is
 * drawn under the last, so the picture is the row's history with time
 * running down the screen.  A cell's neighbours are the two cells on
 * either side of it, four in all: a dead cell with two or three of them
 * alive comes alive, and a live cell with two or four survives.  The
 * original prints `*' and space on a terminal, 64 cells wide, and
 * starts a new random row every 25 generations.
 *
 * This one draws on the framebuffer as well as the terminal, in pages
 * that fill top to bottom or as a scroll with the newest generation at
 * the bottom, and colours a cell by how long it has lived or, as the
 * original, all in one.  `life1d --help' lists the options.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "demo.h"
#include "dterm.h"

/* Cells read beyond each end of the row: the neighbourhood's reach. */
#define PAD		2

/* Ages the colours tell apart.  Older cells share the last colour, and
 * by then they are part of something still or slowly repeating, which
 * reads as one long run of it anyway. */
#define AGE_LEVELS	24

/*
 * Longest a keypress waits to be noticed.  The runtime polls the
 * keyboard between frames, and a demo with nothing due for a whole
 * generation would otherwise spend that time spinning on the poll.
 */
#define IDLE_SLICE_US	50000

/* Defaults, each stated once for both the state and the help. */
#define DEFAULT_CELLS	64	/* the original's row */
#define DEFAULT_DENSITY	50
#define DEFAULT_PAUSE_S	3
/* The default pace fills a screen in this many seconds whatever the
 * cell size, so a picture of small cells moves as briskly as one of
 * big ones. */
#define SCREEN_S	2

#define STR(x)		#x
#define XSTR(x)		STR(x)

/* Worst-case bytes one terminal cell costs: an fg/bg colour pair and a
 * three-byte glyph. */
#define CELL_BYTES_MAX	40

/* ── Palettes ─────────────────────────────────────────────────────── */
/*
 * Ramps of xterm colour indices, newborn first, so one table serves the
 * terminal directly and the framebuffer's colormap through
 * demo_xterm_rgb.  One colour for every live cell is the ramp's first.
 */

static const uint8_t pal_green[] = { 46, 40, 34, 28, 22 };
static const uint8_t pal_amber[] = { 214, 178, 172, 136, 130, 94, 58 };
static const uint8_t pal_white[] = { 231, 255, 252, 249, 246, 243, 240, 237 };
static const uint8_t pal_fire[] = {
	231, 229, 227, 226, 220, 214, 208, 202, 196, 160, 124, 88, 52,
};
static const uint8_t pal_ocean[] = {
	195, 159, 123, 87, 51, 45, 39, 33, 27, 21, 20, 19, 18, 17,
};
static const uint8_t pal_rainbow[] = {
	196, 202, 208, 214, 220, 226, 190, 154, 118, 82, 46, 47, 48,
	49, 50, 51, 45, 39, 33, 27, 21, 57, 93, 129, 165, 201,
};

struct palette {
	const char	*name;
	const uint8_t	*stops;
	unsigned	nstops;
	const char	*desc;
};

#define PAL(name, t, desc)	{ name, t, sizeof(t), desc }

static const struct palette palettes[] = {
	PAL("green", pal_green, "green phosphor, fading darker with age"),
	PAL("amber", pal_amber, "amber phosphor, fading darker with age"),
	PAL("white", pal_white, "white phosphor, fading grey with age"),
	PAL("fire", pal_fire, "white-hot newborns cooling through red"),
	PAL("ocean", pal_ocean, "pale cyan newborns sinking to deep blue"),
	PAL("rainbow", pal_rainbow, "red through the hues to magenta"),
};
#define NPALETTES	(sizeof(palettes) / sizeof(palettes[0]))

static const struct palette *
find_palette(const char *name)
{
	for (unsigned i = 0; i < NPALETTES; i++)
		if (strcmp(palettes[i].name, name) == 0)
			return &palettes[i];
	return NULL;
}

/* ── State ────────────────────────────────────────────────────────── */

enum layout { LAYOUT_PAGE, LAYOUT_SCROLL };
enum paint { PAINT_MONO, PAINT_AGE };
enum edges { EDGES_DEAD, EDGES_WRAP };

struct life;

/*
 * What a surface does with the picture.  The picture is the newest
 * `rows' generations held in a ring, and drawing it is incremental:
 * draw shows positions [drawn, count), and scroll moves everything up
 * by `gens' generations, after which whatever it disturbed has to be
 * drawn again.
 */
struct canvas_ops {
	int	(*setup)(struct life *, const struct demo_surface *);
	void	(*clear)(struct life *);
	void	(*scroll)(struct life *, unsigned gens);
	void	(*draw)(struct life *);
};

struct life {
	/* Options. */
	enum layout		layout;
	enum paint		paint;
	enum edges		edges;
	const struct palette	*pal;
	unsigned		cells_wanted;
	int			rate;		/* generations a second;
						 * 0 runs unpaced, -1 until
						 * decided */
	unsigned		density;	/* percent alive in a new row */
	int			reseed;		/* generations a seed runs;
						 * -1 until decided */
	unsigned		pause_s;
	uint32_t		seed;

	/* The row.  Each pointer is cell 0 of a buffer with PAD cells
	 * either side. */
	unsigned		n;
	uint8_t			*prev, *cur, *next;
	uint8_t			*age;
	uint32_t		rng;
	unsigned		since_seed;
	unsigned		stale;		/* generations repeating with a
						 * period of one or two */
	int			reseed_due;
	int			reseed_per_page;

	/* The picture: display levels, 0 dead and 1..AGE_LEVELS alive. */
	const struct canvas_ops	*ops;
	const struct demo_surface *s;
	unsigned		rows;		/* generations it shows */
	unsigned		per_line;	/* generations a scroll moves */
	uint8_t			*hist;		/* rows x n, a ring */
	unsigned		head, count, drawn;

	/* Pacing. */
	uint64_t		elapsed_us, base_us, stepped;
	unsigned		max_per_frame;
	int			paused;
	uint64_t		resume_us;

	/* Pixel surface: cell size in pixels and where the picture sits. */
	unsigned		cs, ox, oy;
	/* Terminal: the first column, and each level's colour. */
	unsigned		col0;
	int			colour[AGE_LEVELS + 1];
	/* One expanded pixel row, or one terminal line's bytes. */
	uint8_t			*line;
};

static struct life state = {
	.layout = LAYOUT_PAGE,
	.paint = PAINT_AGE,
	.edges = EDGES_DEAD,
	.cells_wanted = DEFAULT_CELLS,
	.rate = -1,
	.density = DEFAULT_DENSITY,
	.reseed = -1,
	.pause_s = DEFAULT_PAUSE_S,
};

static const uint8_t *
hist_row(const struct life *L, unsigned pos)
{
	return &L->hist[(size_t)((L->head + pos) % L->rows) * L->n];
}

/* ── The automaton ────────────────────────────────────────────────── */

static uint32_t
rng_next(struct life *L)
{
	uint32_t x = L->rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	L->rng = x;
	return x;
}

static void
seed_row(struct life *L)
{
	for (unsigned i = 0; i < L->n; i++) {
		/* A percentage by multiply-and-shift, which spends no
		 * divide on every cell. */
		L->cur[i] = ((uint64_t)rng_next(L) * 100 >> 32) < L->density;
		L->age[i] = L->cur[i];
	}
	/* Equal to no row of 0s and 1s, so the first comparisons after a
	 * seed find nothing repeating. */
	memset(L->prev - PAD, 0xFF, L->n + 2 * PAD);
	L->since_seed = 0;
	L->stale = 0;
	L->reseed_due = 0;
}

/* What the neighbourhood sees beyond the ends of the row. */
static void
fill_edges(const struct life *L, uint8_t *row)
{
	unsigned n = L->n;

	if (L->edges == EDGES_WRAP) {
		row[-2] = row[n - 2];
		row[-1] = row[n - 1];
		row[n] = row[0];
		row[n + 1] = row[1];
	} else {
		row[-2] = row[-1] = row[n] = row[n + 1] = 0;
	}
}

/*
 * One generation: next[i] for every cell 0 <= i < n, from cur[i-2]
 * through cur[i+2].  Cells are 0 or 1.  fill_edges has already put the
 * edge policy into the PAD cells beyond each end of cur, so every read
 * is in bounds and neither end needs a case of its own.
 */
static void
life_step(const uint8_t *cur, uint8_t *next, unsigned n)
{
	/* Signed, as the neighbours at i - 2 and i - 1 are the padding
	 * before cell 0. */
	for (int i = 0; i < (int)n; i++) {
		int neighbors = 0;

		neighbors += cur[i - 2];
		neighbors += cur[i - 1];
		neighbors += cur[i + 1];
		neighbors += cur[i + 2];

		if (cur[i]) {
			switch (neighbors) {
			case 2:
			case 4:
				next[i] = 1;
				break;
			default:
				next[i] = 0;
				break;
			}
		} else {
			switch (neighbors) {
			case 2:
			case 3:
				next[i] = 1;
				break;
			default:
				next[i] = 0;
				break;
			}
		}
	}
}

/* Advance the row by one generation, or replace it with a seed. */
static void
advance(struct life *L)
{
	unsigned live = 0;
	uint8_t *t;

	if (L->reseed_due ||
	    (L->reseed > 0 && L->since_seed >= (unsigned)L->reseed)) {
		seed_row(L);
		return;
	}

	fill_edges(L, L->cur);
	life_step(L->cur, L->next, L->n);

	for (unsigned i = 0; i < L->n; i++) {
		if (!L->next[i])
			L->age[i] = 0;
		else if (L->age[i] < UINT8_MAX)
			L->age[i]++;
		live += L->next[i];
	}

	/* A row that has died, or that has shown the same one or two
	 * states for a whole screen, has nothing left to draw. */
	if (memcmp(L->next, L->cur, L->n) == 0 ||
	    memcmp(L->next, L->prev, L->n) == 0)
		L->stale++;
	else
		L->stale = 0;
	if (live == 0 || L->stale >= L->rows)
		L->reseed_due = 1;

	t = L->prev;
	L->prev = L->cur;
	L->cur = L->next;
	L->next = t;
	L->since_seed++;
}

static uint8_t
level_of(const struct life *L, unsigned i)
{
	if (!L->cur[i])
		return 0;
	if (L->paint == PAINT_MONO)
		return 1;
	if (L->age[i] < AGE_LEVELS)
		return L->age[i];
	return AGE_LEVELS;
}

/* Add the current row to the picture, scrolling if it is full.  A page
 * never arrives here full: run() pauses it first. */
static void
push(struct life *L)
{
	uint8_t *dst;

	if (L->count == L->rows) {
		L->head = (L->head + L->per_line) % L->rows;
		L->count -= L->per_line;
		L->ops->scroll(L, L->per_line);
	}
	dst = &L->hist[(size_t)((L->head + L->count) % L->rows) * L->n];
	for (unsigned i = 0; i < L->n; i++)
		dst[i] = level_of(L, i);
	L->count++;
}

static void
new_page(struct life *L)
{
	L->head = L->count = L->drawn = 0;
	L->ops->clear(L);
	if (L->reseed_per_page)
		L->reseed_due = 1;
}

/* ── Pacing ───────────────────────────────────────────────────────── */

static void
idle(uint64_t us)
{
	if (us > IDLE_SLICE_US)
		us = IDLE_SLICE_US;
	usleep((useconds_t)us);
}

static void
run(struct life *L, uint32_t dt_us)
{
	uint64_t due, next_us;

	L->elapsed_us += dt_us;
	if (L->paused) {
		if (L->elapsed_us < L->resume_us) {
			idle(L->resume_us - L->elapsed_us);
			return;
		}
		L->paused = 0;
		new_page(L);
		L->base_us = L->elapsed_us;
		L->stepped = 0;
	}

	if (L->rate == 0) {
		/* Unpaced: one line's worth a frame, so that every
		 * generation is drawn, a scroll included, and the frame
		 * loop alone sets the speed. */
		due = L->stepped + L->per_line;
	} else {
		due = (L->elapsed_us - L->base_us) * (uint64_t)L->rate /
		    1000000u;
		/* Further behind than one frame may make up means the
		 * surface cannot keep the rate: run as fast as it can,
		 * never in bursts. */
		if (due > L->stepped + L->max_per_frame)
			L->stepped = due - L->max_per_frame;
	}
	while (L->stepped < due) {
		if (L->layout == LAYOUT_PAGE && L->count == L->rows) {
			L->paused = 1;
			L->resume_us = L->elapsed_us +
			    (uint64_t)L->pause_s * 1000000u;
			break;
		}
		advance(L);
		push(L);
		L->stepped++;
	}
	L->ops->draw(L);

	if (L->rate == 0 || L->paused)
		return;
	next_us = L->base_us + (L->stepped + 1) * 1000000u / (uint64_t)L->rate;
	if (next_us > L->elapsed_us)
		idle(next_us - L->elapsed_us);
}

static int
start(struct life *L, const struct demo_surface *s,
    const struct canvas_ops *ops)
{
	size_t rowbytes;

	L->s = s;
	L->ops = ops;
	if (ops->setup(L, s) != 0)
		return -1;
	if (L->rows < 2 * L->per_line) {
		fprintf(stderr, "life1d: the surface has room for %u "
		    "generations, too few to show any history\n", L->rows);
		return -1;
	}
	/* Decided only now, as it depends on how many generations the
	 * surface shows. */
	if (L->rate < 0)
		L->rate = (int)((L->rows + SCREEN_S - 1) / SCREEN_S);

	rowbytes = L->n + 2 * PAD;
	L->prev = calloc(rowbytes, 1);
	L->cur = calloc(rowbytes, 1);
	L->next = calloc(rowbytes, 1);
	L->age = calloc(L->n, 1);
	L->hist = calloc((size_t)L->rows, L->n);
	if (L->prev == NULL || L->cur == NULL || L->next == NULL ||
	    L->age == NULL || L->hist == NULL) {
		perror("life1d");
		return -1;
	}
	L->prev += PAD;
	L->cur += PAD;
	L->next += PAD;

	L->reseed_per_page = (L->layout == LAYOUT_PAGE && L->reseed < 0);
	if (L->reseed < 0)
		L->reseed = 0;
	/* xorshift has a fixed point at zero, which a seed of 0 would
	 * land on; the multiply spreads small seeds over the state. */
	L->rng = L->seed * 2654435761u + 1;
	if (L->rng == 0)
		L->rng = 1;

	new_page(L);
	seed_row(L);
	push(L);
	L->ops->draw(L);
	return 0;
}

/* ── Framebuffer ──────────────────────────────────────────────────── */

static void
pixel_cmap(const struct life *L, const struct demo_surface *s)
{
	const struct palette *p = L->pal;
	uint8_t r[256], g[256], b[256];

	/* Every pixel has a colour, so dead ground has to be one. */
	memset(r, 0, sizeof(r));
	memset(g, 0, sizeof(g));
	memset(b, 0, sizeof(b));

	/* Blended between stops rather than snapped onto them: there are
	 * more ages than most palettes have stops, and a colour per age
	 * keeps a cell's ageing a gradient instead of bands. */
	for (unsigned lv = 1; lv <= AGE_LEVELS; lv++) {
		unsigned pos = (lv - 1) * (p->nstops - 1) * 256 /
		    (AGE_LEVELS - 1);
		unsigned stop = pos >> 8, frac = pos & 0xFF;
		unsigned stop1 = (stop + 1 < p->nstops) ? stop + 1 : stop;
		uint8_t r0, g0, b0, r1, g1, b1;

		demo_xterm_rgb(p->stops[stop], &r0, &g0, &b0);
		demo_xterm_rgb(p->stops[stop1], &r1, &g1, &b1);
		r[lv] = (uint8_t)((r0 * (256 - frac) + r1 * frac) >> 8);
		g[lv] = (uint8_t)((g0 * (256 - frac) + g1 * frac) >> 8);
		b[lv] = (uint8_t)((b0 * (256 - frac) + b1 * frac) >> 8);
	}
	demo_set_cmap(s, r, g, b);
}

static int
pixel_setup(struct life *L, const struct demo_surface *s)
{
	if (s->cmap_entries <= AGE_LEVELS) {
		fprintf(stderr, "life1d: the framebuffer has %u colours, "
		    "fewer than the %u ages it shows\n", s->cmap_entries,
		    AGE_LEVELS + 1);
		return -1;
	}
	/* Square cells, as many pixels a side as the width allows, and
	 * the picture centred in what is left over. */
	L->n = (L->cells_wanted < s->width) ? L->cells_wanted : s->width;
	L->cs = s->width / L->n;
	L->rows = s->height / L->cs;
	L->ox = (s->width - L->n * L->cs) / 2;
	L->oy = (s->height - L->rows * L->cs) / 2;
	L->per_line = 1;
	/* Every generation a frame makes up costs only its computation
	 * and one row of the redraw, so a frame may catch up a screen. */
	L->max_per_frame = L->rows;
	L->line = malloc((size_t)L->n * L->cs);
	if (L->line == NULL) {
		perror("life1d");
		return -1;
	}
	pixel_cmap(L, s);
	return 0;
}

static void
pixel_clear(struct life *L)
{
	const struct demo_surface *s = L->s;
	uint8_t *dst = s->pix + (size_t)L->oy * s->stride + L->ox;

	for (unsigned y = 0; y < L->rows * L->cs; y++, dst += s->stride)
		memset(dst, 0, (size_t)L->n * L->cs);
}

/* With no scroll register every row moves, so all of them are drawn
 * again — from the history in RAM, as the aperture is never read. */
static void
pixel_scroll(struct life *L, unsigned gens)
{
	(void)gens;
	L->drawn = 0;
}

static void
pixel_draw(struct life *L)
{
	const struct demo_surface *s = L->s;
	size_t width = (size_t)L->n * L->cs;

	for (; L->drawn < L->count; L->drawn++) {
		const uint8_t *px = hist_row(L, L->drawn);
		uint8_t *dst;

		/* A level is its own pixel value, so at one pixel a cell
		 * the history row already is the pixel row. */
		if (L->cs > 1) {
			uint8_t *p = L->line;

			for (unsigned i = 0; i < L->n; i++)
				for (unsigned k = 0; k < L->cs; k++)
					*p++ = px[i];
			px = L->line;
		}
		dst = s->pix + (size_t)(L->oy + L->drawn * L->cs) *
		    s->stride + L->ox;
		for (unsigned k = 0; k < L->cs; k++, dst += s->stride)
			memcpy(dst, px, width);
	}
}

static const struct canvas_ops pixel_ops = {
	pixel_setup, pixel_clear, pixel_scroll, pixel_draw,
};

/* ── Terminal ─────────────────────────────────────────────────────── */
/*
 * One column a cell.  In half-blocks a terminal line holds two
 * generations, which makes the cells square; in plain characters it
 * holds one, `*' or space, as the original prints them.  Lines span
 * the whole surface, the margins drawn dead, so a line scrolled in
 * leaves no seam whatever colour the terminal erases it in.
 */

static void
emit(const char *buf, size_t len)
{
	(void)write(STDOUT_FILENO, buf, len);
}

static void
cell_line(struct life *L, unsigned l)
{
	const struct demo_surface *s = L->s;
	unsigned p = l * L->per_line;
	const uint8_t *top = NULL, *bot = NULL;
	int prev_t = -1, prev_b = -1;
	char *out = (char *)L->line;

	if (p < L->count)
		top = hist_row(L, p);
	if (L->per_line == 2 && p + 1 < L->count)
		bot = hist_row(L, p + 1);

	out = dterm_at(out, (int)l + 1, 1);
	for (unsigned c = 0; c < s->width; c++) {
		int inside = (c >= L->col0 && c - L->col0 < L->n);
		unsigned i = c - L->col0;
		int t, b;

		if (!s->blocks) {
			*out++ = (inside && top != NULL && top[i]) ? '*' : ' ';
			continue;
		}
		t = L->colour[(inside && top != NULL) ? top[i] : 0];
		b = L->colour[(inside && bot != NULL) ? bot[i] : 0];
		/* Runs of one colour pair, dead ground above all, are
		 * the bulk of a line: send the colours only when they
		 * change. */
		if (t != prev_t || b != prev_b) {
			out = dterm_pair_color(out, t, b);
			prev_t = t;
			prev_b = b;
		}
		out = dterm_pair_glyph(out);
	}
	emit((char *)L->line, (size_t)(out - (char *)L->line));
}

static int
cell_setup(struct life *L, const struct demo_surface *s)
{
	const struct palette *p = L->pal;

	L->n = (L->cells_wanted < s->width) ? L->cells_wanted : s->width;
	L->col0 = (s->width - L->n) / 2;
	L->per_line = s->blocks ? 2 : 1;
	L->rows = s->height * L->per_line;
	/* Each generation is a line on the wire, which is what limits
	 * the rate here: a frame draws one line's worth and no more, so
	 * a keypress is never stuck behind a screenful of catching up. */
	L->max_per_frame = L->per_line;

	L->colour[0] = DTERM_BLACK;
	for (unsigned lv = 1; lv <= AGE_LEVELS; lv++)
		L->colour[lv] = p->stops[(lv - 1) * p->nstops / AGE_LEVELS];

	L->line = malloc((size_t)s->width * CELL_BYTES_MAX + 32);
	if (L->line == NULL) {
		perror("life1d");
		return -1;
	}
	if (L->layout == LAYOUT_SCROLL) {
		char buf[32];
		char *out = dterm_scroll_region(buf, (int)s->height);

		emit(buf, (size_t)(out - buf));
	}
	return 0;
}

static void
cell_clear(struct life *L)
{
	for (unsigned l = 0; l < L->s->height; l++)
		cell_line(L, l);
}

/* Inside the scroll region a newline on its last row moves the whole
 * picture up a line, and the terminal does the copying. */
static void
cell_scroll(struct life *L, unsigned gens)
{
	char buf[32];
	char *out = dterm_at(buf, (int)L->s->height, 1);

	for (unsigned k = 0; k < gens / L->per_line; k++)
		*out++ = '\n';
	emit(buf, (size_t)(out - buf));
	L->drawn = (L->drawn > gens) ? L->drawn - gens : 0;
}

static void
cell_draw(struct life *L)
{
	if (L->drawn >= L->count)
		return;
	/* From the line holding the first generation not yet shown,
	 * which in half-blocks may already show the other one. */
	for (unsigned l = L->drawn / L->per_line;
	    l <= (L->count - 1) / L->per_line; l++)
		cell_line(L, l);
	L->drawn = L->count;
}

static const struct canvas_ops cell_ops = {
	cell_setup, cell_clear, cell_scroll, cell_draw,
};

/* ── Runtime glue ─────────────────────────────────────────────────── */

static void
frame_pixels(const struct demo_surface *s, uint32_t dt_us, uint64_t frame,
    void *vs)
{
	struct life *L = vs;

	if (frame == 0 && start(L, s, &pixel_ops) != 0)
		exit(1);
	run(L, dt_us);
}

static void
frame_cells(const struct demo_surface *s, uint32_t dt_us, uint64_t frame,
    void *vs)
{
	struct life *L = vs;

	if (frame == 0 && start(L, s, &cell_ops) != 0)
		exit(1);
	run(L, dt_us);
}

static int
parse_uint(const char *opt, const char *v, unsigned long long lo,
    unsigned long long hi, unsigned long long *out)
{
	unsigned long long x;
	char *end;

	if (v == NULL) {
		fprintf(stderr, "life1d: %s needs a value\n", opt);
		return -1;
	}
	errno = 0;
	x = strtoull(v, &end, 10);
	if (v[0] < '0' || v[0] > '9' || *end != '\0' || errno != 0 ||
	    x < lo || x > hi) {
		fprintf(stderr, "life1d: %s wants %llu..%llu, not `%s'\n",
		    opt, lo, hi, v);
		return -1;
	}
	*out = x;
	return 0;
}

static void
list_palettes(void)
{
	for (unsigned i = 0; i < NPALETTES; i++)
		printf("  %-10s %s\n", palettes[i].name, palettes[i].desc);
}

static int
parse_args(int argc, char **argv, void *vs)
{
	struct life *L = vs;
	int seed_given = 0;

	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
		unsigned long long x;

		if (strcmp(a, "--page") == 0) {
			L->layout = LAYOUT_PAGE;
			continue;
		}
		if (strcmp(a, "--scroll") == 0) {
			L->layout = LAYOUT_SCROLL;
			continue;
		}

		/* Everything else takes a value. */
		i++;
		if (strcmp(a, "--paint") == 0 && v != NULL &&
		    strcmp(v, "mono") == 0) {
			L->paint = PAINT_MONO;
		} else if (strcmp(a, "--paint") == 0 && v != NULL &&
		    strcmp(v, "age") == 0) {
			L->paint = PAINT_AGE;
		} else if (strcmp(a, "--paint") == 0) {
			fprintf(stderr, "life1d: --paint is mono or age\n");
			return -1;
		} else if (strcmp(a, "--edges") == 0 && v != NULL &&
		    strcmp(v, "dead") == 0) {
			L->edges = EDGES_DEAD;
		} else if (strcmp(a, "--edges") == 0 && v != NULL &&
		    strcmp(v, "wrap") == 0) {
			L->edges = EDGES_WRAP;
		} else if (strcmp(a, "--edges") == 0) {
			fprintf(stderr, "life1d: --edges is dead or wrap\n");
			return -1;
		} else if (strcmp(a, "--palette") == 0 && v != NULL &&
		    strcmp(v, "list") == 0) {
			list_palettes();
			exit(0);
		} else if (strcmp(a, "--palette") == 0) {
			if (v == NULL) {
				fprintf(stderr, "life1d: --palette needs a "
				    "value\n");
				return -1;
			}
			L->pal = find_palette(v);
			if (L->pal == NULL) {
				fprintf(stderr, "life1d: no palette `%s'; "
				    "`--palette list' shows them\n", v);
				return -1;
			}
		} else if (strcmp(a, "--cells") == 0) {
			if (parse_uint(a, v, 2 * PAD, 4096, &x) != 0)
				return -1;
			L->cells_wanted = (unsigned)x;
		} else if (strcmp(a, "--rate") == 0 && v != NULL &&
		    strcmp(v, "max") == 0) {
			L->rate = 0;
		} else if (strcmp(a, "--rate") == 0) {
			if (parse_uint(a, v, 1, 1000, &x) != 0)
				return -1;
			L->rate = (int)x;
		} else if (strcmp(a, "--density") == 0) {
			if (parse_uint(a, v, 1, 100, &x) != 0)
				return -1;
			L->density = (unsigned)x;
		} else if (strcmp(a, "--seed") == 0) {
			if (parse_uint(a, v, 0, UINT32_MAX, &x) != 0)
				return -1;
			L->seed = (uint32_t)x;
			seed_given = 1;
		} else if (strcmp(a, "--reseed") == 0) {
			if (parse_uint(a, v, 0, 1000000, &x) != 0)
				return -1;
			L->reseed = (int)x;
		} else if (strcmp(a, "--pause") == 0) {
			if (parse_uint(a, v, 0, 3600, &x) != 0)
				return -1;
			L->pause_s = (unsigned)x;
		} else {
			fprintf(stderr, "life1d: unknown option `%s' "
			    "(surface options go first; see --help)\n", a);
			return -1;
		}
	}

	if (L->pal == NULL && L->paint == PAINT_MONO)
		L->pal = find_palette("green");
	else if (L->pal == NULL)
		L->pal = find_palette("fire");
	/* Told, so that a run worth seeing again can be. */
	if (!seed_given) {
		L->seed = (uint32_t)time(NULL) ^ (uint32_t)getpid();
		fprintf(stderr, "life1d: --seed %u\n", (unsigned)L->seed);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	static const struct demo d = {
		.name = "life1d",
		.frame_cell = frame_cells,
		.frame_pixel = frame_pixels,
		.parse = parse_args,
		.usage_tail = "[--page | --scroll] [--paint mono|age]\n"
		    "       [--palette NAME|list] [--cells N] [--rate N|max] "
		    "[--density PCT]\n"
		    "       [--seed N] [--reseed N] [--edges dead|wrap] "
		    "[--pause SECONDS]",
		.help =
		    "  --page               fill the screen top to bottom, "
		    "then start a new page\n"
		    "                       (default)\n"
		    "  --scroll             newest generation at the bottom, "
		    "history scrolling up\n"
		    "  --paint mono|age     one colour for every live cell, "
		    "or colour by how many\n"
		    "                       generations it has lived "
		    "(default age)\n"
		    "  --palette NAME|list  the colours; `list' names them "
		    "(default green for\n"
		    "                       mono, fire for age)\n"
		    "  --cells N            cells in the row, at most what "
		    "the surface fits\n"
		    "                       (default " XSTR(DEFAULT_CELLS) ")\n"
		    "  --rate N|max         generations a second, or max for "
		    "as fast as every\n"
		    "                       one can be drawn "
		    "(default a screenful every " XSTR(SCREEN_S) " seconds)\n"
		    "  --density PCT        cells alive in a new random row "
		    "(default " XSTR(DEFAULT_DENSITY) ")\n"
		    "  --seed N             random seed, to repeat a run "
		    "(default from the clock,\n"
		    "                       and printed)\n"
		    "  --reseed N           a new random row every N "
		    "generations, 0 never\n"
		    "                       (default each page, never when "
		    "scrolling)\n"
		    "  --edges dead|wrap    beyond the row's ends: dead cells, "
		    "or the other end\n"
		    "                       (default dead)\n"
		    "  --pause SECONDS      how long a full page stays up "
		    "(default " XSTR(DEFAULT_PAUSE_S) ")\n"
		    "\n"
		    "A row that dies out, or shows the same one or two states "
		    "for a whole screen,\n"
		    "is always replaced by a new random one.\n",
		.state = &state,
	};

	return demo_main(argc, argv, &d);
}

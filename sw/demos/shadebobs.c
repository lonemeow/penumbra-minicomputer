/* shadebobs.c - Amiga-style shadebobs demo.
 *
 * Multiple sprite "bobs" follow lissajous-ish paths around the screen,
 * each painting a small soft splat into a per-pixel intensity buffer
 * as it moves.  Intensities accumulate over time, so cells that bobs
 * visit frequently grow brighter — the trails build up into a glowing
 * colorful pattern that fills the picture gradually instead of all
 * at once.
 *
 * Per-frame I/O cost is *low*: only the cells touched this frame get
 * re-emitted (a handful of cells per bob × a handful of bobs).  No
 * full-screen redraw, so the demo runs cleanly under the 115200-baud
 * UART ceiling — typical frame is ~1 KB on the wire.
 *
 * Per-frame compute cost is also low: 2 sin-table lookups per bob to
 * compute its new position, plus splat updates (a 3x3 box of byte
 * increments per bob).  No multiplies on the hot path.
 *
 * Usage: shadebobs [-m|--mono | -b|--blocks] [-n|--nodraw]
 *                  [FRAMES] [WIDTH] [HEIGHT]
 *   defaults: blocks on TTY / mono when piped, infinite frames,
 *             78 x 39 cells
 *
 * Press any key or Ctrl-C to stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <termios.h>

#include "demo.h"
#include "dterm.h"

/* ── Sin lookup table (built from libm at startup) ──────────────────── */

static int8_t sin_lut[256];

static void init_sin_lut(void) {
    for (int i = 0; i < 256; i++) {
        double theta = 2.0 * M_PI * (double)i / 256.0;
        sin_lut[i] = (int8_t)lround(sin(theta) * 127.0);
    }
}

/* ── Palette (rainbow), brightness-indexed ──────────────────────────── */

/* A 30-entry rainbow palette identical to the one used in mandelbrot/
 * julia/plasma.  Indexed here by per-cell intensity (0..255 → 0..29),
 * so brighter cells land on later positions in the hue cycle. */
static const uint8_t palette[] = {
     16,  17,  18,  19,  20,  21,         /* dark blue start            */
     27,  33,  39,  45,  51,               /* blue → cyan                */
     50,  82, 118, 154, 190,               /* cyan → green → yellow      */
    220, 214, 208, 202, 196,               /* yellow → orange → red      */
    197, 198, 199, 200, 201,               /* red → magenta              */
    165, 129,  93,                          /* magenta → bright magenta   */
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))

/* Map an intensity byte (0..255) to a palette index.  Quantizes the
 * 256-step intensity range across the 30-entry palette.  Stays as a
 * bit-shift table lookup — no divides on the hot path. */
static uint8_t intensity_to_color(int intensity) {
    /* (intensity * PALETTE_LEN) >> 8 — keeps the math in shifts/adds. */
    int idx = (intensity * (int)PALETTE_LEN) >> 8;
    if (idx >= (int)PALETTE_LEN) idx = (int)PALETTE_LEN - 1;
    return palette[idx];
}

/* ── Precomputed decimal strings for 0..255 ─────────────────────────── */




/* ── Measurement instrumentation ────────────────────────────────────── */

static inline void emit_bytes(const char *buf, size_t n) {
    (void)write(STDOUT_FILENO, buf, n);
}

/* ── Intensity buffer + cell emit ───────────────────────────────────── */

/* intensity[col * pixel_rows + row] — saturating uint8_t per half-pixel.
 * Cells get re-emitted whenever a bob splats into them this frame. */
static uint8_t *intensity;
static int g_width;
static int g_pixel_rows;

/* Set on a pixel surface: the intensity byte is the palette index, so
 * an accumulating splat can store its new value straight to the
 * mapping instead of anything re-reading the buffer later. */
static uint8_t *fb_pix;
static unsigned fb_stride;

/* Bookkeeping for which cells need re-emission this frame.  Allocated
 * once with max possible touched cells (bobs * splat_cells_per_bob);
 * cleared at the start of each frame, populated during splat. */
struct dirty_cell {
    int16_t col;   /* cell column, 0..width-1 */
    int16_t row;   /* cell row,    0..height-1 */
};
static struct dirty_cell *dirty;
static int dirty_count;
static int dirty_capacity;

/* Mark a half-pixel as dirty by adding its containing cell to the
 * dirty list (dedup with a flag bitmap to avoid duplicate emits when
 * multiple splat updates fall in the same cell). */
static uint8_t *dirty_flag;     /* one byte per cell, set ⇒ in dirty list */

static void mark_dirty(int col, int pixel_row) {
    int cell_row = pixel_row >> 1;
    int cell_idx = col * (g_pixel_rows >> 1) + cell_row;
    if (dirty_flag[cell_idx]) return;
    dirty_flag[cell_idx] = 1;
    if (dirty_count < dirty_capacity) {
        dirty[dirty_count].col = (int16_t)col;
        dirty[dirty_count].row = (int16_t)cell_row;
        dirty_count++;
    }
}

/* Saturating add: intensity capped at 255 so a hot cell stays at the
 * brightest palette entry rather than wrapping. */
static inline void splat_pixel(int col, int row, int weight) {
    if (col < 0 || col >= g_width || row < 0 || row >= g_pixel_rows) return;
    int idx = col * g_pixel_rows + row;
    int v = intensity[idx] + weight;
    if (v > 255) v = 255;
    intensity[idx] = (uint8_t)v;
    if (fb_pix != NULL)
        fb_pix[(size_t)row * fb_stride + col] = (uint8_t)v;
    else
        mark_dirty(col, row);
}

/* Center-heavy splat weights: a single visit adds a soft bump, many
 * visits accumulate into a saturated core inside a softer halo.
 *
 * Two sizes, because a bob's apparent size is a fraction of the
 * picture and the two surfaces differ by four times in each axis.  The
 * 3x3 that reads as a soft blob on a character grid is a speck on
 * 320x240; the 7x7 keeps roughly the same share of the screen. */
#define SPLAT_MAX_R  3

static const int splat_small[3][3] = {
    {2,  3, 2},
    {3, 12, 3},
    {2,  3, 2},
};

static const int splat_large[7][7] = {
    {0, 1, 1, 2, 1, 1, 0},
    {1, 1, 2, 3, 2, 1, 1},
    {1, 2, 4, 6, 4, 2, 1},
    {2, 3, 6, 12, 6, 3, 2},
    {1, 2, 4, 6, 4, 2, 1},
    {1, 1, 2, 3, 2, 1, 1},
    {0, 1, 1, 2, 1, 1, 0},
};

static int splat_r = 1;   /* set from the surface at setup */

static void splat(int x, int y) {
    for (int dy = -splat_r; dy <= splat_r; dy++) {
        for (int dx = -splat_r; dx <= splat_r; dx++) {
            int w = (splat_r == 1)
                ? splat_small[dy + 1][dx + 1]
                : splat_large[dy + splat_r][dx + splat_r];

            if (w != 0)
                splat_pixel(x + dx, y + dy, w);
        }
    }
}

/* Emit one cell as half-block with fg=top intensity color, bg=bottom
 * intensity color.  Cursor positioned absolutely; full escape per
 * cell since cells in the dirty list aren't necessarily contiguous. */
static void emit_cell_blocks(int col, int cell_row) {
    int top_i = intensity[col * g_pixel_rows + (cell_row << 1)];
    int bot_i = intensity[col * g_pixel_rows + (cell_row << 1) + 1];
    int fg = intensity_to_color(top_i);
    int bg = intensity_to_color(bot_i);

    char buf[48];
    char *out = buf;

    /* \033[ROW;COLH */
    out = dterm_at(out, cell_row + 1, col + 1);

    out = dterm_pair(out, fg, bg);

    emit_bytes(buf, (size_t)(out - buf));
}

/* Mono mode: brightness ramp by intensity.  Uses just the top half-
 * pixel intensity (bottom half ignored since one cell = one char). */
static const char ramp[] = " .:-=+*#%@";
#define RAMP_LEN  (sizeof(ramp) - 1)

static void emit_cell_mono(int col, int cell_row) {
    int top_i = intensity[col * g_pixel_rows + (cell_row << 1)];
    int idx = (top_i * (int)(RAMP_LEN - 1)) >> 8;
    if (idx >= (int)(RAMP_LEN - 1)) idx = (int)(RAMP_LEN - 1);

    char buf[16];
    char *out = buf;
    out = dterm_at(out, cell_row + 1, col + 1);
    *out++ = ramp[idx];
    emit_bytes(buf, (size_t)(out - buf));
}

/* ── Bobs ───────────────────────────────────────────────────────────── */

/* Each bob follows a lissajous path with independent x/y frequencies.
 * The frequency ratios produce closed curves; choosing co-prime pairs
 * gives interesting non-trivial figure-8 / star / rosette shapes. */
struct bob {
    int amp_x, amp_y;          /* path amplitude in pixels        */
    int cx, cy;                /* path center in pixels           */
    uint16_t fx, fy;           /* phase step per tick, in 1/256ths */
    uint8_t phx, phy;          /* phase offsets                   */
};

#define NUM_BOBS  5

static struct bob bobs[NUM_BOBS];

static void init_bobs(int width, int pixel_rows) {
    int cx = width / 2;
    int cy = pixel_rows / 2;
    int ax = (width - 4) / 2;
    int ay = (pixel_rows - 4) / 2;

    /* Lissajous paths spend disproportionate time at their turning
     * points (where sin'(t) → 0), so a bob with full-screen amplitude
     * dwells noticeably at the edges.  Fix: shrink each bob's reach
     * to ~50% of the screen and offset its center so the four corner
     * bobs orbit their own quadrants, with a fifth bob roaming the
     * middle.  Turning points still happen, but now they're spread
     * across the picture instead of clustering at the actual edges. */
    int sax = ax / 2;           /* shrunk amplitude per bob */
    int say = ay / 2;
    int ox  = ax / 3;           /* off-center offset for quadrant bobs */
    int oy  = ay / 3;

    /* Frequencies in 1/256 lookup steps per tick, chosen co-prime and
     * not near a simple ratio of one another, so no two bobs
     * synchronize and no single path closes quickly.  Phases spread
     * around the circle so they do not start bunched. */
    bobs[0] = (struct bob){ sax, say, cx - ox, cy - oy,  97, 149,   0,   0 };
    bobs[1] = (struct bob){ sax, say, cx + ox, cy - oy, 131, 103,  50,  30 };
    bobs[2] = (struct bob){ sax, say, cx - ox, cy + oy, 113, 181, 100,  70 };
    bobs[3] = (struct bob){ sax, say, cx + ox, cy + oy, 167, 127, 150, 110 };
    bobs[4] = (struct bob){ sax, say, cx,      cy,       89, 157, 200, 150 };
}

static inline void bob_position(const struct bob *b, uint32_t t,
                                int *out_x, int *out_y) {
    /* Phase is carried at 1/256 of a lookup step, so a frequency need
     * not be a whole number of steps per tick.  With small integer
     * ratios the lissajous figure closes in a couple of hundred ticks
     * and the repeat is obvious once the picture is large enough to
     * see it; at this resolution the curve takes minutes to come back
     * to where it started. */
    uint32_t px = (uint32_t)b->phx * 256u + (uint32_t)b->fx * t;
    uint32_t py = (uint32_t)b->phy * 256u + (uint32_t)b->fy * t;
    int sx = sin_lut[(px >> 8) & 0xFF];
    int sy = sin_lut[(py >> 8) & 0xFF];

    /* sin_lut is [-127, 127]; (amp * sin) >> 7 lands in [-amp, amp]
     * without a divide. */
    *out_x = b->cx + ((b->amp_x * sx) >> 7);
    *out_y = b->cy + ((b->amp_y * sy) >> 7);
}

/* ── Time base ──────────────────────────────────────────────────────── */
/*
 * The bobs move along their paths at a fixed rate in wall-clock time
 * rather than one step per frame.  A frame costs wildly different
 * amounts on the two surfaces — a terminal frame is bounded by what
 * fits down the wire, a framebuffer frame by a few hundred stores — so
 * stepping per frame would make the same demo crawl on one and race on
 * the other.
 */
/* Ticks per second of wall clock.  The phase step is a fraction of a
 * lookup entry, so this sets how fast a bob travels its path rather
 * than how far it jumps per frame. */
#define BOB_TICKS_PER_SEC  700

struct shadebobs {
    uint64_t elapsed_us;    /* since the first frame */
    int      ready;         /* buffers sized to the surface */
};

static struct shadebobs state;

/*
 * Sizing waits for the first frame because that is when the surface is
 * known: the runtime picks one after arguments are parsed, and dt == 0
 * marks the frame where nothing has happened yet.
 */
static int setup(const struct demo_surface *s) {
    int cells;

    g_width = (int)s->width;
    g_pixel_rows = (s->kind == DEMO_PIXEL) ? (int)s->height
                                           : (int)s->height * 2;
    cells = g_width * (g_pixel_rows >> 1);

    /* A bob should cover a similar share of the picture on either
     * surface, and the pixel one is four times the extent in each
     * axis. */
    splat_r = (s->kind == DEMO_PIXEL) ? SPLAT_MAX_R : 1;

    intensity = calloc((size_t)g_width * g_pixel_rows, 1);
    /* Worst case: every splatted half-pixel lands in its own cell. */
    dirty_capacity = NUM_BOBS * 9;
    dirty = calloc((size_t)dirty_capacity, sizeof(*dirty));
    dirty_flag = calloc((size_t)cells, 1);
    if (intensity == NULL || dirty == NULL || dirty_flag == NULL) {
        perror("calloc");
        return -1;
    }

    init_sin_lut();
    init_bobs(g_width, g_pixel_rows);
    state.ready = 1;
    return 0;
}

/* Advance every bob to where wall-clock time says it should be, and
 * splat there.  Returns nothing: the marks land in the intensity
 * buffer, and on a pixel surface in the framebuffer too. */
static void step_bobs(uint64_t elapsed_us) {
    uint32_t t = (uint32_t)((elapsed_us * BOB_TICKS_PER_SEC) / 1000000ull);

    for (int i = 0; i < NUM_BOBS; i++) {
        int bx, by;

        bob_position(&bobs[i], t, &bx, &by);
        splat(bx, by);
    }
}

/* ── Cell surface ───────────────────────────────────────────────────── */

static void frame_cells(const struct demo_surface *s, uint32_t dt_us,
                        uint64_t frame, void *vs) {
    struct shadebobs *sb = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
    }
    sb->elapsed_us += dt_us;

    /* Only cells touched this frame are re-emitted, which is what keeps
     * a frame inside the serial console's budget: a handful of cells
     * per bob rather than a full screen. */
    for (int i = 0; i < dirty_count; i++) {
        int cell_idx = dirty[i].col * (g_pixel_rows >> 1) + dirty[i].row;
        dirty_flag[cell_idx] = 0;
    }
    dirty_count = 0;

    step_bobs(sb->elapsed_us);

    for (int i = 0; i < dirty_count; i++) {
        if (s->blocks)
            emit_cell_blocks(dirty[i].col, dirty[i].row);
        else
            emit_cell_mono(dirty[i].col, dirty[i].row);
    }
}

/* ── Pixel surface ──────────────────────────────────────────────────── */

/*
 * The intensity byte is the palette index, so there is no conversion
 * step at all: splat writes the new value straight through to the
 * mapping as it accumulates.  Nothing scans the picture per frame, and
 * the dirty list the cell path needs has nothing to do here.
 */
static void build_cmap(const struct demo_surface *s) {
    uint8_t r[256], g[256], b[256];

    unsigned n = s->cmap_entries < 256 ? s->cmap_entries : 256;

    /* Intensity 0 is untouched ground and must be black.  On a terminal
     * that happens by omission — an untouched cell is simply never
     * emitted — but every pixel here has a colour, so the background
     * has to be one. */
    r[0] = g[0] = b[0] = 0;

    /*
     * Interpolate between the palette's stops rather than quantising
     * onto them.  The terminal has 30 colours to spend and the steps
     * between them are the best it can do; here there are 256 entries,
     * and stepping straight from one stop to the next wastes them —
     * the eye reads the result as bands with a jump at each boundary
     * instead of a gradient.  Blending across the gap spreads each
     * transition over the entries between the stops.
     */
    for (unsigned i = 1; i < n; i++) {
        /* Where this entry falls along the ramp, and how far between
         * the two stops that surround it. */
        unsigned pos = (i - 1) * (PALETTE_LEN - 1) * 256 / (n - 1);
        unsigned stop = pos >> 8, frac = pos & 0xFF;
        uint8_t r0, g0, b0, r1, g1, b1;

        demo_xterm_rgb(palette[stop], &r0, &g0, &b0);
        demo_xterm_rgb(palette[stop + 1 < PALETTE_LEN ? stop + 1 : stop],
            &r1, &g1, &b1);
        r[i] = (uint8_t)((r0 * (256 - frac) + r1 * frac) >> 8);
        g[i] = (uint8_t)((g0 * (256 - frac) + g1 * frac) >> 8);
        b[i] = (uint8_t)((b0 * (256 - frac) + b1 * frac) >> 8);
    }
    demo_set_cmap(s, r, g, b);
}

static void frame_pixels(const struct demo_surface *s, uint32_t dt_us,
                         uint64_t frame, void *vs) {
    struct shadebobs *sb = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
        build_cmap(s);
        fb_pix = s->pix;
        fb_stride = s->stride;
    }
    sb->elapsed_us += dt_us;
    step_bobs(sb->elapsed_us);
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name        = "shadebobs",
        .frame_cell  = frame_cells,
        .frame_pixel = frame_pixels,
        .state       = &state,
    };

    return demo_main(argc, argv, &d);
}

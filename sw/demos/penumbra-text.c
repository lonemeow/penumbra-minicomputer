/* penumbra-text.c - "Penumbra" logo screensaver.
 *
 * Combines the shadebobs intensity-buffer engine with a static path
 * defined by a 6x11 bitmap font of P/E/N/U/M/B/R/A.  A single "bob"
 * teleports from text pixel to text pixel in the stored order,
 * splatting the same 3x3 weight pattern shadebobs uses (center 64,
 * edges 8, corners 4) at each stop.  After several full traversal
 * passes the text pixels saturate; the halo bleed builds a softer
 * glow around the letters.
 *
 * Animation cycles in two phases:
 *   TRACE   the bob walks the text pixels in order, accumulating
 *           intensity along the way.  Multiple passes (4 by
 *           default) accumulate to saturation, so the text emerges
 *           dim → medium → bright over the course of the cycle.
 *   HOLD    once the text is fully lit, the picture stays bright
 *           for a few seconds so visitors can read the logo.
 * After HOLD, the screen is cleared and the cycle restarts.
 *
 * Per-frame I/O is small: ~6-12 unique cells touched per frame
 * during TRACE (BOB_SPEED splats × 3x3 halo), zero cells during
 * HOLD.  Ideal for the idle-loop screensaver slot in the VCF kiosk
 * — slow build-up, calm reveal, clean reset.
 *
 * Usage: penumbra-text [-m|--mono | -b|--blocks] [-n|--nodraw]
 *                      [FRAMES] [WIDTH] [HEIGHT]
 *   defaults: blocks on TTY / mono when piped, infinite frames,
 *             78 x 39 cells
 *
 * Press any key or Ctrl-C to stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>     /* sin() at startup for the plasma underline */
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <termios.h>

#include "demo.h"
#include "dterm.h"

/* ── Heated-metal palette ───────────────────────────────────────────
 *
 * Black → dark red → red → orange → gold → yellow → white.  Linear
 * progression of brightness with hue shifting smoothly through warm
 * tones.  Intensity 0 maps to black (color 16) so unlit text cells
 * are invisible against the terminal background. */
static const uint8_t palette[] = {
     16,   /* 0:    black             */
     52,   /*       darkest red       */
     88,   /*       dark red          */
    124,   /*       red               */
    160,   /*       brighter red      */
    196,   /*       full red          */
    202,   /*       orange-red        */
    208,   /*       orange            */
    214,   /*       bright orange     */
    220,   /*       gold              */
    226,   /*       yellow            */
    228,   /*       pale yellow       */
    230,   /*       very pale         */
    231,   /* 255:  white-hot         */
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))

static uint8_t intensity_to_color(int intensity) {
    int idx = (intensity * (int)PALETTE_LEN) >> 8;
    if (idx >= (int)PALETTE_LEN) idx = (int)PALETTE_LEN - 1;
    return palette[idx];
}

/* ── 1D plasma underline ────────────────────────────────────────────
 *
 * A single-row demoscene plasma effect below the logo: each column's
 * color comes from sin_lut[(x*3 + t) & 0xff] + sin_lut[(x*5 + t*3) & 0xff],
 * sampling a rainbow palette through a precomputed [v + 254] table.
 *
 * The lookup table eliminates the divide that would otherwise happen
 * per pixel per frame (mapping v in [-254, 254] onto rainbow indices),
 * which would be a software __divsi3 each call — far too expensive
 * for the inner loop at 25 MHz. */
static int8_t sin_lut[256];

static void init_sin_lut(void) {
    for (int i = 0; i < 256; i++) {
        double theta = 2.0 * M_PI * (double)i / 256.0;
        sin_lut[i] = (int8_t)lround(sin(theta) * 127.0);
    }
}

static const uint8_t rainbow[] = {
    196, 202, 208, 214, 220, 226,
    190, 154, 118,  82,  46,
     47,  48,  49,  50,  51,
     45,  39,  33,  27,  21,
     57,  93, 129, 165, 201,
    200, 199, 198, 197,
};
#define RAINBOW_LEN  (sizeof(rainbow) / sizeof(rainbow[0]))

static uint8_t plasma_color_lut[512];

static void init_plasma_lut(void) {
    /* Index i corresponds to v = i - 254, in range [-254, 254].
     * Map evenly onto the rainbow palette indices. */
    for (int i = 0; i < 512; i++) {
        int idx = (i * (int)RAINBOW_LEN) >> 9;   /* / 512 */
        if (idx >= (int)RAINBOW_LEN) idx = (int)RAINBOW_LEN - 1;
        plasma_color_lut[i] = rainbow[idx];
    }
}

/* ── Precomputed ASCII decimals for 0..255 ──────────────────────────── */




/* ── Measurement instrumentation ────────────────────────────────────── */

static inline void emit_bytes(const char *buf, size_t n) {
    (void)write(STDOUT_FILENO, buf, n);
}

/* ── Intensity buffer + cell emit ───────────────────────────────────── */

static uint8_t *intensity;
static int g_width;
static int g_pixel_rows;

/* Set on a pixel surface: the intensity byte indexes the colormap
 * directly, scaled into the glow's share of it. */
static uint8_t *fb_pix;
static unsigned fb_stride;
static unsigned fb_glow_last;
static int g_cell_rows;     /* = g_pixel_rows in mono, / 2 in blocks */

/* Dirty-cell list with flag bitmap for dedup. */
struct dirty_cell {
    int16_t col;
    int16_t row;
};
static struct dirty_cell *dirty;
static int dirty_count;
static int dirty_capacity;
static uint8_t *dirty_flag;

static void mark_dirty(int col, int pixel_row) {
    int cell_row = (g_pixel_rows == g_cell_rows * 2) ? (pixel_row >> 1)
                                                     : pixel_row;
    int cell_idx = col * g_cell_rows + cell_row;
    if (dirty_flag[cell_idx]) return;
    dirty_flag[cell_idx] = 1;
    if (dirty_count < dirty_capacity) {
        dirty[dirty_count].col = (int16_t)col;
        dirty[dirty_count].row = (int16_t)cell_row;
        dirty_count++;
    }
}

static inline void splat_pixel(int col, int row, int weight) {
    if (col < 0 || col >= g_width || row < 0 || row >= g_pixel_rows) return;
    int idx = col * g_pixel_rows + row;
    int v = intensity[idx] + weight;
    if (v > 255) v = 255;
    intensity[idx] = (uint8_t)v;
    if (fb_pix != NULL)
        fb_pix[(size_t)row * fb_stride + col] =
            (uint8_t)((v * fb_glow_last) >> 8);
    else
        mark_dirty(col, row);
}

static void emit_cell_blocks(int col, int cell_row) {
    int top_i = intensity[col * g_pixel_rows + (cell_row << 1)];
    int bot_i = intensity[col * g_pixel_rows + (cell_row << 1) + 1];
    int fg = intensity_to_color(top_i);
    int bg = intensity_to_color(bot_i);

    char buf[48];
    char *out = buf;

    out = dterm_at(out, cell_row + 1, col + 1);

    out = dterm_pair(out, fg, bg);

    emit_bytes(buf, (size_t)(out - buf));
}

static const char ramp[] = " .:-=+*#%@";
#define RAMP_LEN  (sizeof(ramp) - 1)

/* ── Plasma row geometry + emitter ──────────────────────────────────
 *
 * One cell row (2 pixels in blocks mode), positioned a couple of
 * pixel rows below the text.  Width slightly wider than the text
 * itself, so it reads as a deliberate decorative underline rather
 * than coincidentally matching. */

#define PLASMA_GAP_PIXELS  3   /* gap between text bottom and plasma top */
#define PLASMA_PAD_COLS    2   /* extra columns on each side of text */

static int plasma_cell_row;    /* cell row containing the plasma band   */
static int plasma_row_px;      /* the same band as a pixel row          */
static int plasma_x0;          /* leftmost plasma column (screen coord) */
static int plasma_width;       /* number of columns covered             */
static int plasma_active;      /* 1 if plasma fits on screen, 0 if not  */

static void emit_plasma_line(int t, int mode_is_blocks) {
    if (!plasma_active) return;

    /* Build the entire plasma row into one buffer, then write once.
     * RLE on the bg-color escape so contiguous same-color columns
     * share an escape. */
    char buf[1024];
    char *out = buf;

    /* Position cursor at the row's first column. */
    out = dterm_at(out, plasma_cell_row + 1, plasma_x0 + 1);

    if (mode_is_blocks) {
        int last_color = -1;
        for (int x = 0; x < plasma_width; x++) {
            int v = sin_lut[(uint8_t)(x * 3 + t)]
                  + sin_lut[(uint8_t)(x * 5 + t * 3)];
            int color = plasma_color_lut[v + 254];
            if (color != last_color) {
                out = dterm_bg(out, color);
                last_color = color;
            }
            *out++ = ' ';   /* bg color shows through the space cell */
        }
        /* Reset SGR at end so the bg doesn't leak into subsequent
         * cursor-position-then-no-color emits elsewhere on screen. */
        *out++ = '\033'; *out++ = '['; *out++ = '0'; *out++ = 'm';
    } else {
        /* Mono: emit a brightness-ramp character per column, no color. */
        for (int x = 0; x < plasma_width; x++) {
            int v = sin_lut[(uint8_t)(x * 3 + t)]
                  + sin_lut[(uint8_t)(x * 5 + t * 3)];
            int idx = ((v + 254) * (int)(RAMP_LEN - 1)) >> 9;
            if (idx < 0) idx = 0;
            if (idx >= (int)(RAMP_LEN - 1)) idx = (int)(RAMP_LEN - 1);
            *out++ = ramp[idx];
        }
    }

    emit_bytes(buf, (size_t)(out - buf));
}

static void emit_cell_mono(int col, int cell_row) {
    int top_i = intensity[col * g_pixel_rows + cell_row];
    int idx = (top_i * (int)(RAMP_LEN - 1)) >> 8;
    if (idx >= (int)(RAMP_LEN - 1)) idx = (int)(RAMP_LEN - 1);

    char buf[16];
    char *out = buf;
    out = dterm_at(out, cell_row + 1, col + 1);
    *out++ = ramp[idx];
    emit_bytes(buf, (size_t)(out - buf));
}

/* ── Bitmap font: P, E, N, U, M, B, R, A in 6w x 11h ────────────────
 *
 * Each row stores bits 5..0 left-to-right for the 6 columns.  Bit 5
 * is the leftmost pixel.  A '1' means lit, '0' means background. */

#define LETTER_W    6
#define LETTER_H    11
#define LETTER_GAP  1
#define NUM_LETTERS 8

static const uint8_t glyphs[NUM_LETTERS][LETTER_H] = {
    /* P */ { 0x3e, 0x21, 0x21, 0x21, 0x3e, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
    /* E */ { 0x3f, 0x20, 0x20, 0x20, 0x3e, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3f },
    /* N */ { 0x21, 0x31, 0x31, 0x29, 0x29, 0x25, 0x25, 0x23, 0x23, 0x23, 0x21 },
    /* U */ { 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x1e },
    /* M */ { 0x21, 0x33, 0x33, 0x2d, 0x2d, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21 },
    /* B */ { 0x3e, 0x21, 0x21, 0x21, 0x3e, 0x21, 0x21, 0x21, 0x21, 0x21, 0x3e },
    /* R */ { 0x3e, 0x21, 0x21, 0x21, 0x3e, 0x28, 0x24, 0x22, 0x21, 0x21, 0x21 },
    /* A */ { 0x1e, 0x21, 0x21, 0x21, 0x3f, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21 },
};

/* Flat list of every lit pixel in the rendered "PENUMBRA" text, in
 * screen pixel coordinates.  Built once at startup. */
struct text_pixel { int16_t x, y; };
static struct text_pixel *text_pixels;
static int text_pixel_count;
static int text_x0;
static int text_width_px;

/*
 * The glyphs are a 6x11 bitmap, which fills a character grid and is a
 * postage stamp on a framebuffer four times the extent in each axis.
 *
 * Scaling multiplies the path rather than spacing it out: each glyph
 * pixel becomes a scale x scale block of points the bob walks through.
 * Leaving the points scale apart instead would ask the splat to bridge
 * the gaps, and a kernel wide enough to do that deposits its energy
 * over so much area that the picture washes out before the strokes
 * join — the text reads as a row of dots with a haze around it.
 * Walking every pixel keeps the splat local, exactly as it is on a
 * character grid.
 */
static int text_scale = 1;

static void build_text_pixels(int screen_w, int screen_h, int scale) {
    int lw = LETTER_W * scale, lh = LETTER_H * scale;
    int lgap = LETTER_GAP * scale;

    text_scale = scale;
    text_width_px = NUM_LETTERS * lw + (NUM_LETTERS - 1) * lgap;
    text_x0    = (screen_w - text_width_px) / 2;
    int text_y0 = (screen_h - lh) / 2;
    if (text_x0  < 0) text_x0  = 0;
    if (text_y0  < 0) text_y0  = 0;

    /* Plasma underline geometry.  Sits below the text, slightly wider.
     * Kept in both spaces: the cell renderer addresses a cell row, the
     * pixel renderer a pixel row, and one is not the other. */
    int plasma_pixel_y = text_y0 + lh + PLASMA_GAP_PIXELS * scale;
    plasma_row_px = plasma_pixel_y;
    plasma_cell_row = plasma_pixel_y >> 1;
    plasma_x0 = text_x0 - PLASMA_PAD_COLS;
    plasma_width = text_width_px + 2 * PLASMA_PAD_COLS;
    if (plasma_x0 < 0) {
        plasma_width += plasma_x0;
        plasma_x0 = 0;
    }
    if (plasma_x0 + plasma_width > screen_w) {
        plasma_width = screen_w - plasma_x0;
    }
    plasma_active = (plasma_pixel_y + 1 < screen_h) && (plasma_width > 0);

    plasma_active = plasma_active && (plasma_pixel_y + scale < screen_h);

    int cap = NUM_LETTERS * LETTER_W * LETTER_H * scale * scale;
    text_pixels = malloc(sizeof(*text_pixels) * (size_t)cap);
    if (!text_pixels) { perror("malloc"); exit(1); }
    text_pixel_count = 0;
    for (int letter = 0; letter < NUM_LETTERS; letter++) {
        int lx = text_x0 + letter * (lw + lgap);
        for (int row = 0; row < LETTER_H; row++) {
            uint8_t bits = glyphs[letter][row];
            for (int col = 0; col < LETTER_W; col++) {
                if (bits & (uint8_t)(1u << (LETTER_W - 1 - col))) {
                    /* The whole block, so the stroke is a stroke. */
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            text_pixels[text_pixel_count].x =
                                (int16_t)(lx + col * scale + sx);
                            text_pixels[text_pixel_count].y =
                                (int16_t)(text_y0 + row * scale + sy);
                            text_pixel_count++;
                        }
                    }
                }
            }
        }
    }
}

/* ── Bob splat (3x3 center-heavy weight pattern) ────────────────────
 *
 * Same weighting as shadebobs: the center cell gets the bulk of the
 * intensity per visit, edges get less, corners least.  Repeated visits
 * accumulate (via splat_pixel's saturating add) so the text pixels
 * progress toward full brightness while the halo cells stay dimmer. */
/* Heavier than the shadebobs kernel: the text saturates in a handful
 * of passes rather than accumulating slowly.  Two sizes, since a stroke
 * should cover the same share of the picture on a surface four times
 * the extent in each axis. */
#define SPLAT_MAX_R  3

static const int splat_small[3][3] = {
    { 4,  8,  4 },
    { 8, 64,  8 },
    { 4,  8,  4 },
};

/*
 * Wider than the 3x3 so the halo spreads in proportion to the larger
 * picture, and much lower — with the path walking every pixel, a
 * stroke pixel collects the whole kernel over a pass, so the sum is
 * what decides how many passes saturate it.  Roughly 60 gives the
 * four passes MAX_PASSES assumes; the peak alone would have saturated
 * a stroke in the first one.
 */
static const int splat_large[7][7] = {
    { 0,  1,  1,  1,  1,  1,  0 },
    { 1,  1,  1,  2,  1,  1,  1 },
    { 1,  1,  2,  4,  2,  1,  1 },
    { 1,  2,  4,  6,  4,  2,  1 },
    { 1,  1,  2,  4,  2,  1,  1 },
    { 1,  1,  1,  2,  1,  1,  1 },
    { 0,  1,  1,  1,  1,  1,  0 },
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

/* ── Animation state machine ────────────────────────────────────────── */

enum phase {
    PHASE_TRACE,    /* bob walks the text pixels, splatting at each */
    PHASE_HOLD,     /* text saturated, holding the bright picture   */
};
static int phase;
static int path_idx;    /* index into text_pixels[]              */
static int pass_num;    /* completed full-traversal passes       */

/*
 * Rates in wall-clock terms rather than per frame.  The two surfaces
 * differ by more than an order of magnitude in how long a frame takes,
 * so a trace measured in pixels-per-frame would crawl on one and blur
 * past on the other.
 */
/* How long one full traversal takes, rather than a rate per point:
 * scaling the glyphs multiplies the path, and the logo should still
 * draw itself in the same few seconds. */
#define PASS_US          3600000ull
#define MAX_PASSES             4
#define HOLD_US          4000000ull /* how long the finished logo stays */

/* Plasma band phase, in the same lookup units the LUT is indexed by. */
#define PLASMA_TICKS_PER_SEC  20

struct ptext {
    uint64_t elapsed_us;
    uint64_t phase_started_us;  /* when the current phase began */
    uint64_t traced_us;         /* time spent tracing this cycle */
    uint64_t visited;           /* path points walked this cycle */
};

static struct ptext state;

/* Reset the screen and start a fresh trace cycle.
 *
 * \033[0m goes *before* \033[2J: erase escapes fill with the current
 * background colour, and after a trace frame that is whatever the last
 * cell set — often a bright one.  Without the reset the screen flips to
 * that colour and stays there. */
static void reset_cells(struct ptext *p) {
    static const char clear_esc[] = "\033[0m\033[2J\033[H";

    memset(intensity, 0, (size_t)g_width * (size_t)g_pixel_rows);
    emit_bytes(clear_esc, sizeof(clear_esc) - 1);
    phase = PHASE_TRACE;
    path_idx = 0;
    pass_num = 0;
    p->phase_started_us = p->elapsed_us;
    p->traced_us = 0;
    p->visited = 0;
}

static void reset_pixels(struct ptext *p, const struct demo_surface *s) {
    memset(intensity, 0, (size_t)g_width * (size_t)g_pixel_rows);
    memset(s->pix, 0, (size_t)s->stride * s->height);
    phase = PHASE_TRACE;
    path_idx = 0;
    pass_num = 0;
    p->phase_started_us = p->elapsed_us;
    p->traced_us = 0;
    p->visited = 0;
}

/*
 * Walk the bob to where elapsed time says it should be.  Splatting is
 * cumulative, so this advances to the target index rather than moving
 * by a per-frame step: a slow frame covers more ground, and the logo
 * takes the same few seconds to draw itself either way.
 */
static void step_trace(struct ptext *p) {
    uint64_t want;

    if (phase == PHASE_HOLD)
        return;

    /*
     * Counted as one continuous walk rather than a position within the
     * current pass.  Tracking a within-pass target skips the tail of
     * every pass: the frame that crosses a boundary sees the target
     * wrap back near zero, and the pixels between where the walk had
     * got to and the end of the path are never visited — the last
     * letter never gets drawn.
     */
    p->traced_us = p->elapsed_us - p->phase_started_us;
    want = (p->traced_us * (uint64_t)text_pixel_count) / PASS_US;
    if (want > (uint64_t)MAX_PASSES * text_pixel_count)
        want = (uint64_t)MAX_PASSES * text_pixel_count;

    while (p->visited < want) {
        int i = (int)(p->visited % (uint64_t)text_pixel_count);

        splat(text_pixels[i].x, text_pixels[i].y);
        p->visited++;
    }
    pass_num = (int)(p->visited / (uint64_t)text_pixel_count);

    if (pass_num >= MAX_PASSES) {
        phase = PHASE_HOLD;
        p->phase_started_us = p->elapsed_us;
    }
}

/* ── Setup ──────────────────────────────────────────────────────────── */

static int setup(const struct demo_surface *s) {
    int cells, scale;

    g_width = (int)s->width;
    g_pixel_rows = (s->kind == DEMO_PIXEL) ? (int)s->height
                                           : (int)s->height * 2;
    g_cell_rows = (s->kind == DEMO_PIXEL) ? (int)s->height : (int)s->height;
    cells = g_width * ((s->kind == DEMO_PIXEL) ? 1 : (int)s->height);

    intensity = calloc((size_t)g_width * g_pixel_rows, 1);
    dirty_capacity = g_width * g_cell_rows;
    dirty = calloc((size_t)dirty_capacity, sizeof(*dirty));
    dirty_flag = calloc((size_t)cells, 1);
    if (intensity == NULL || dirty == NULL || dirty_flag == NULL) {
        perror("calloc");
        return -1;
    }

    /* Scale the glyphs to the surface, and take a splat radius that
     * covers the gap the scaling opens between strokes. */
    scale = g_pixel_rows / 60;
    if (scale < 1)
        scale = 1;
    if (scale > 4)
        scale = 4;
    splat_r = (scale > 1) ? SPLAT_MAX_R : 1;

    init_sin_lut();
    init_plasma_lut();
    build_text_pixels(g_width, g_pixel_rows, scale);
    return 0;
}

/*
 * The colormap carries two ramps that have nothing to do with each
 * other: the glow's intensity and the plasma band's rainbow.  A
 * terminal keeps them apart for free by naming a colour per cell; here
 * there is one table, so it is split — the low entries are the glow,
 * the high ones the band, and each side scales into its own range.
 */
#define PIX_GLOW_LAST     191
#define PIX_PLASMA_FIRST  192

static void build_cmap(const struct demo_surface *s) {
    uint8_t r[256], g[256], b[256];
    unsigned n = s->cmap_entries < 256 ? s->cmap_entries : 256;

    memset(r, 0, sizeof(r));
    memset(g, 0, sizeof(g));
    memset(b, 0, sizeof(b));

    /* Glow: interpolated between the palette's stops, so the ramp
     * gradates rather than stepping between 30 fixed colours. */
    for (unsigned i = 1; i <= PIX_GLOW_LAST && i < n; i++) {
        unsigned pos = (i - 1) * (PALETTE_LEN - 1) * 256 / PIX_GLOW_LAST;
        unsigned stop = pos >> 8, frac = pos & 0xFF;
        uint8_t r0, g0, b0, r1, g1, b1;

        demo_xterm_rgb(palette[stop], &r0, &g0, &b0);
        demo_xterm_rgb(palette[stop + 1 < PALETTE_LEN ? stop + 1 : stop],
            &r1, &g1, &b1);
        r[i] = (uint8_t)((r0 * (256 - frac) + r1 * frac) >> 8);
        g[i] = (uint8_t)((g0 * (256 - frac) + g1 * frac) >> 8);
        b[i] = (uint8_t)((b0 * (256 - frac) + b1 * frac) >> 8);
    }

    /* Band: the rainbow, spread over what is left. */
    for (unsigned i = PIX_PLASMA_FIRST; i < n; i++) {
        unsigned k = (i - PIX_PLASMA_FIRST) * RAINBOW_LEN
            / (n - PIX_PLASMA_FIRST);

        demo_xterm_rgb(rainbow[k < RAINBOW_LEN ? k : RAINBOW_LEN - 1],
            &r[i], &g[i], &b[i]);
    }
    demo_set_cmap(s, r, g, b);
}

/* ── Cell surface ───────────────────────────────────────────────────── */

static void frame_cells(const struct demo_surface *s, uint32_t dt_us,
                        uint64_t frame, void *vs) {
    struct ptext *p = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
        reset_cells(p);
    }
    p->elapsed_us += dt_us;

    for (int i = 0; i < dirty_count; i++) {
        int cell_idx = dirty[i].col * g_cell_rows + dirty[i].row;
        dirty_flag[cell_idx] = 0;
    }
    dirty_count = 0;

    step_trace(p);

    for (int i = 0; i < dirty_count; i++) {
        if (s->blocks)
            emit_cell_blocks(dirty[i].col, dirty[i].row);
        else
            emit_cell_mono(dirty[i].col, dirty[i].row);
    }

    /* The band animates throughout, including while the logo holds —
     * it is what says the picture is alive rather than frozen. */
    if (plasma_active)
        emit_plasma_line((int)((p->elapsed_us * PLASMA_TICKS_PER_SEC)
            / 1000000ull), s->blocks);

    if (phase == PHASE_HOLD && p->elapsed_us - p->phase_started_us >= HOLD_US)
        reset_cells(p);
}

/* ── Pixel surface ──────────────────────────────────────────────────── */

/* The band as a row of pixels rather than a row of cells: same LUT,
 * same phase, written through the colormap's upper range. */
static void draw_plasma_pixels(const struct demo_surface *s, int t) {
    unsigned band_h = (unsigned)text_scale;
    unsigned y0 = (unsigned)plasma_row_px;

    if (!plasma_active || band_h == 0)
        return;
    for (unsigned y = y0; y < y0 + band_h && y < s->height; y++) {
        uint8_t *row = s->pix + (size_t)y * s->stride;

        for (int i = 0; i < plasma_width; i++) {
            int x = plasma_x0 + i;
            uint8_t v = plasma_color_lut[(uint8_t)(x * 3 + t)];

            if (x >= 0 && (unsigned)x < s->width)
                row[x] = (uint8_t)(PIX_PLASMA_FIRST
                    + (v % (256 - PIX_PLASMA_FIRST)));
        }
    }
}

static void frame_pixels(const struct demo_surface *s, uint32_t dt_us,
                         uint64_t frame, void *vs) {
    struct ptext *p = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
        build_cmap(s);
        fb_pix = s->pix;
        fb_stride = s->stride;
        fb_glow_last = PIX_GLOW_LAST;
        reset_pixels(p, s);
    }
    p->elapsed_us += dt_us;

    step_trace(p);
    draw_plasma_pixels(s, (int)((p->elapsed_us * PLASMA_TICKS_PER_SEC)
        / 1000000ull));

    if (phase == PHASE_HOLD && p->elapsed_us - p->phase_started_us >= HOLD_US)
        reset_pixels(p, s);
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name        = "penumbra-text",
        .frame_cell  = frame_cells,
        .frame_pixel = frame_pixels,
        .state       = &state,
    };

    return demo_main(argc, argv, &d);
}

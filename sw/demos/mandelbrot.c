/* mandelbrot.c - fixed-point Mandelbrot set renderer + ALU benchmark.
 *
 * Renders the Mandelbrot set into an ASCII grid using Q4.28 fixed-point
 * arithmetic.  Penumbra has no FP hardware (FP is software-emulated),
 * so doing this in integer math both runs in reasonable time and acts
 * as a clean stress test for the 32x32 -> 64-bit multiply path and the
 * surrounding ALU.
 *
 * Q4.28 format:
 *   bit 31    : sign
 *   bits 30..28 : integer part (effective range +-8)
 *   bits 27..0  : 28 fractional bits (~3.7e-9 resolution)
 *
 * Multiplication: (int64_t)a * (int64_t)b yields a Q8.56 result; shift
 * right by 28 to renormalize back to Q4.28.  Addition/subtraction are
 * straight 32-bit signed ops.
 *
 * Output: WIDTH x HEIGHT ASCII grid with a density ramp where lighter
 * characters mean "escaped early" (outside the set) and '@' means
 * "still bounded after MAX_ITER iterations" (inside, or close to it).
 *
 * Usage: mandelbrot [-m|--mono | -b|--blocks | -f|--fb [DEV]]
 *                   [-H|--hold SECONDS] [PRESET]
 *                   [WIDTH] [HEIGHT] [MAX_ITER]
 *        mandelbrot list
 *   defaults: full preset, 78 x 39, per-preset max_iter,
 *             blocks mode on TTY / mono mode when piped
 *
 * --hold applies to fb mode, where the picture would otherwise vanish
 * the instant the render finished: it is how long a finished picture
 * stays up before the console returns.  A keypress always ends it; the
 * default of 0 means only a keypress does, which is what someone at a
 * prompt wants.  An unattended exhibit passes a number of seconds
 * instead, so the display moves on by itself.
 *
 * Render modes:
 *   mono    pure ASCII brightness ramp, no escape codes; works on any
 *           terminal, captures cleanly to a text file
 *   blocks  Unicode U+2580 upper-half-block + ANSI 256-color fg/bg per
 *           cell, doubles effective vertical resolution.  Needs an
 *           xterm-level terminal (most modern emulators qualify).
 *   fb      one sample per pixel into a wsdisplay framebuffer, mapped
 *           straight into the process.  Geometry comes from the device
 *           rather than the WIDTH/HEIGHT arguments.  It renders on
 *           whatever stdout is attached to, so this needs to be run on
 *           a display advertising the capability; DEV names another
 *           one when the picture should go somewhere else.
 *
 * Height defaults to odd so the middle row samples y=cy exactly —
 * that puts the negative-real spike of the full set on a single
 * clean row instead of straddling two off-axis rows.
 *
 * Presets zoom into specific regions of interest (Seahorse Valley,
 * mini-mandelbrot satellites, tight spirals).  Tighter zooms need
 * higher iteration counts to resolve boundary detail, so each
 * preset carries its own default max_iter; the CLI override is for
 * when you want to dial it in by hand.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>     /* isatty() for color auto-detect */
#include "demo.h"
#include "dterm.h"
#include "fractal.h"

/* ── Viewport presets ────────────────────────────────────────────────── */

struct preset {
    const char *name;
    q_t cx, cy;            /* center in the complex plane */
    q_t half_w;            /* half real-axis extent */
    int def_max_iter;      /* recommended cap; tighter zooms need more */
    const char *desc;
};

static const struct preset presets[] = {
    { "full",
      Q_FROM_FRAC(-5, 10),         Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(15, 10),         96,
      "Full set, x in [-2, 1]" },
    { "antenna",
      Q_FROM_FRAC(-85, 100),       Q_FROM_FRAC(45, 100),
      Q_FROM_FRAC(85, 100),        128,
      "Upper hemisphere; cardioid, period-2 bulb, antenna filaments" },
    { "seahorse",
      Q_FROM_FRAC(-745, 1000),     Q_FROM_FRAC(105, 1000),
      Q_FROM_FRAC(40, 1000),       192,
      "Seahorse Valley, between main cardioid and period-2 bulb" },
    { "mini",
      Q_FROM_FRAC(-1750, 1000),    Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(40, 1000),       192,
      "Mini-mandelbrot satellite on the negative real axis" },
    { "elephant",
      Q_FROM_FRAC(275, 1000),      Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(30, 1000),       192,
      "Elephant Valley, right of the main cardioid" },
    { "spiral",
      Q_FROM_FRAC(-74543, 100000), Q_FROM_FRAC(11301, 100000),
      Q_FROM_FRAC(5, 1000),        384,
      "Tight spiral near (-0.74543, 0.11301)" },
};
#define NUM_PRESETS  (sizeof(presets) / sizeof(presets[0]))

static const struct preset *find_preset(const char *name) {
    for (size_t i = 0; i < NUM_PRESETS; i++) {
        if (strcmp(presets[i].name, name) == 0) return &presets[i];
    }
    return NULL;
}

static void list_presets(void) {
    printf("Presets:\n");
    for (size_t i = 0; i < NUM_PRESETS; i++) {
        printf("  %-10s  %s\n", presets[i].name, presets[i].desc);
    }
}

/* ── Mandelbrot kernel ───────────────────────────────────────────────── */

/* Count iterations until z = z^2 + c diverges (|z|^2 > 4), capped at
 * max_iter.  Returns 0..max_iter; max_iter means "still bounded".
 *
 * This is the inner ALU loop of the whole program — every pixel calls
 * it once.  Keep it tight: each iteration is roughly
 *   3 qmul + a few adds + a compare + branch.
 * On a WIDTH*HEIGHT grid with depth D, the worst case is WIDTH*HEIGHT*D
 * multiplies, which for 78*40*96 = ~300 K multiplies per frame. */
static int mandel_iter(q_t cr, q_t ci, int max_iter) {
    q_t zr = 0, zi = 0;

    for (int i = 0; i < max_iter; i++)
    {
        q_t zr_squared = qmul(zr, zr);
        q_t zi_squared = qmul(zi, zi);
        q_t zr_times_zi = qmul(zr, zi);
        q_t zr_new = zr_squared - zi_squared + cr;
        q_t zi_new = zr_times_zi * 2 + ci;

        if (zr_squared + zi_squared > Q_ESCAPE)
            return i;

        zr = zr_new;
        zi = zi_new;
    }

    return max_iter;
}

/* ── Rendering ───────────────────────────────────────────────────────── */

/* Density ramp: lighter = escaped early, '@' = inside the set.
 * Standard 10-character ramp; index by iter * (len-1) / max_iter. */

/* Shared geometry: derives the viewport rectangle from a preset and
 * a target pixel grid.  `cell_height` is always H (drives aspect —
 * physical cell shape is 1 wide x 2 tall regardless of pixel mode).
 * `pixel_rows` is H for mono (one sample per cell) or 2H for blocks
 * (two stacked samples per cell) and drives sample spacing only. */
/* ── State ───────────────────────────────────────────────────────────── */
/*
 * What the demo itself owns: which region to draw and how hard to look
 * at its boundary.  Everything else — surface, geometry, timing, when
 * to stop — belongs to the runtime.
 */
struct mandel {
    const struct preset *preset;
    int                  max_iter;
};

static struct mandel state = { NULL, -1 };

static int parse_args(int argc, char **argv, void *vs) {
    struct mandel *m = vs;
    int i = 0;

    if (i < argc && strcmp(argv[i], "list") == 0) {
        list_presets();
        exit(0);
    }
    if (i < argc && !dterm_looks_like_int(argv[i])) {
        m->preset = find_preset(argv[i]);
        if (m->preset == NULL) {
            fprintf(stderr, "unknown preset '%s' (try `list')\n", argv[i]);
            return -1;
        }
        i++;
    } else {
        m->preset = find_preset("full");
    }
    if (i < argc)
        m->max_iter = atoi(argv[i++]);
    if (m->max_iter < 0)
        m->max_iter = m->preset->def_max_iter;
    if (m->max_iter < 4 || m->max_iter > FRACTAL_MAX_ITER) {
        fprintf(stderr, "max_iter must be between 4 and %d\n",
            FRACTAL_MAX_ITER);
        return -1;
    }
    return 0;
}

/* ── Renderers ───────────────────────────────────────────────────────── */

static void render_mono(const struct demo_surface *s, void *vs) {
    const struct mandel *m = vs;
    const struct demo_viewport v = demo_viewport(s, m->preset->cx,
        m->preset->cy, m->preset->half_w, s->height);
    char *line = malloc((size_t)s->width + 2);

    if (!line) { perror("malloc"); exit(1); }
    for (unsigned py = 0; py < s->height; py++) {
        q_t ci = v.y_min + (q_t)((int64_t)py * v.dy);
        for (unsigned px = 0; px < s->width; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            int it = mandel_iter(cr, ci, m->max_iter);
            int idx = (it >= m->max_iter)
                ? FRACTAL_RAMP_LEN - 1
                : (it * (FRACTAL_RAMP_LEN - 1)) / m->max_iter;
            line[px] = fractal_ramp[idx];
        }
        line[s->width]     = '\n';
        line[s->width + 1] = '\0';
        fputs(line, stdout);
    }
    free(line);
}

/* Two samples per cell: the upper half-block carries the top sample as
 * foreground, the cell's background carries the bottom one. */
static void render_blocks(const struct demo_surface *s, void *vs) {
    const struct mandel *m = vs;
    /* Two vertical samples per cell: emit the upper-half-block glyph
     * with fg = top sample, bg = bottom. */
    const unsigned rows = s->height * 2;
    const struct demo_viewport v = demo_viewport(s, m->preset->cx,
        m->preset->cy, m->preset->half_w, rows);
    /* Worst case per cell: two SGR sequences plus U+2580 in UTF-8. */
    char *line = malloc((size_t)s->width * 32 + 16);

    if (!line) { perror("malloc"); exit(1); }
    for (unsigned cy = 0; cy < s->height; cy++) {
        q_t ci_top = v.y_min + (q_t)((int64_t)(cy * 2)     * v.dy);
        q_t ci_bot = v.y_min + (q_t)((int64_t)(cy * 2 + 1) * v.dy);
        char *out  = line;
        int last_fg = -2, last_bg = -2;   /* force the first emit */

        for (unsigned px = 0; px < s->width; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            int fg = fractal_cell_color(
                mandel_iter(cr, ci_top, m->max_iter), m->max_iter);
            int bg = fractal_cell_color(
                mandel_iter(cr, ci_bot, m->max_iter), m->max_iter);

            /* Run-length the fg/bg pair: a flat band of the same
             * colours costs one escape, not one per cell.  On a
             * 115200-baud console that is the difference between a
             * frame fitting and not. */
            if (fg != last_fg || bg != last_bg) {
                out = dterm_pair_color(out, fg, bg);
                last_fg = fg;
                last_bg = bg;
            }
            out = dterm_pair_glyph(out);
        }
        /* Reset, so the next row does not inherit and the prompt after
         * the picture starts uncoloured. */
        *out++ = '\033'; *out++ = '['; *out++ = '0'; *out++ = 'm';
        *out++ = '\n';
        *out   = '\0';
        fputs(line, stdout);
    }
    free(line);
}

/*
 * One sample per pixel, written straight into the mapping.  No aspect
 * correction beyond what the viewport already did: the device doubles
 * both axes equally, so pixels are square.
 */
static void render_pixels(const struct demo_surface *s, void *vs) {
    const struct mandel *m = vs;
    const struct demo_viewport v = demo_viewport(s, m->preset->cx,
        m->preset->cy, m->preset->half_w, s->height);
    uint8_t r[256], g[256], b[256];

    fractal_build_cmap(r, g, b, s->cmap_entries, m->max_iter);
    demo_set_cmap(s, r, g, b);

    for (unsigned py = 0; py < s->height; py++) {
        q_t ci = v.y_min + (q_t)((int64_t)py * v.dy);
        uint8_t *row = s->pix + (size_t)py * s->stride;

        for (unsigned px = 0; px < s->width; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            row[px] = fractal_pixel_index(mandel_iter(cr, ci, m->max_iter));
        }
    }
}

/* The cell renderer the runtime calls picks its own detail level: the
 * half-block trick needs a terminal that can draw the glyph. */
static void render_cells(const struct demo_surface *s, void *vs) {
    if (s->blocks)
        render_blocks(s, vs);
    else
        render_mono(s, vs);
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name         = "mandelbrot",
        .render_cell  = render_cells,
        .render_pixel = render_pixels,
        .parse        = parse_args,
        .usage_tail   = "[PRESET] [MAX_ITER]\n       mandelbrot list",
        .state        = &state,
    };

    return demo_main(argc, argv, &d);
}

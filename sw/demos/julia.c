/* julia.c - fixed-point Julia set renderer + ALU benchmark.
 *
 * Companion to mandelbrot.c.  Renders Julia sets — the family of
 * fractals you get by fixing c in z_{n+1} = z_n^2 + c and varying
 * the *starting* z_0 across the pixel grid (instead of the other
 * way around, which is the Mandelbrot set).  Same kernel, same
 * cost, completely different family of shapes — each preset's c
 * value gives a wildly different connected/disconnected/dendrite
 * structure.
 *
 * Q4.28 fixed-point throughout, same as mandelbrot.c; see that file
 * for the format and qmul rationale.  The numerics for Julia are
 * actually a touch friendlier than Mandelbrot because z_0 is bounded
 * by the viewport (|z_0| <= half_w) rather than starting from zero
 * and being driven only by c.
 *
 * Usage: julia [-m|--mono | -b|--blocks] [PRESET]
 *              [WIDTH] [HEIGHT] [MAX_ITER]
 *        julia list
 *   defaults: rabbit preset, 78 x 39, per-preset max_iter,
 *             blocks mode on TTY / mono mode when piped
 *
 * Render modes match mandelbrot — mono is pure ASCII, blocks uses
 * U+2580 + ANSI 256-color fg/bg for double vertical resolution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "demo.h"
#include "dterm.h"
#include "fractal.h"

/* ── Julia presets ──────────────────────────────────────────────────── */

/* A preset names a c value (the fixed Julia parameter) and a viewport
 * to look at the z-plane through.  The viewport is usually centered
 * on the origin; the half-width controls zoom.  c values are the
 * iconic ones — Douady rabbit, dendrite, San Marco basilica, Fatou
 * dust, Siegel-disk Julia. */
struct preset {
    const char *name;
    q_t cr, ci;            /* the Julia parameter c = cr + ci*i */
    q_t view_cx, view_cy;  /* viewport center (usually 0, 0) */
    q_t view_hw;           /* viewport half-width */
    int def_max_iter;
    const char *desc;
};

static const struct preset presets[] = {
    { "rabbit",
      Q_FROM_FRAC(-123, 1000),    Q_FROM_FRAC(745, 1000),
      Q_FROM_FRAC(0, 1),          Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(16, 10),        128,
      "Douady rabbit, c = -0.123 + 0.745i — three-eared connected set" },
    { "dendrite",
      Q_FROM_FRAC(-70, 100),      Q_FROM_FRAC(27015, 100000),
      Q_FROM_FRAC(0, 1),          Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(16, 10),        160,
      "Dendrite, c = -0.7 + 0.27i — lightning-bolt filaments" },
    { "basilica",
      Q_FROM_FRAC(-1, 1),         Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(0, 1),          Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(18, 10),        96,
      "San Marco basilica, c = -1.0 — chain of connected blobs" },
    { "dust",
      Q_FROM_FRAC(-835, 1000),    Q_FROM_FRAC(-2321, 10000),
      Q_FROM_FRAC(0, 1),          Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(16, 10),        160,
      "Fatou dust, c = -0.835 - 0.2321i — disconnected speckle pattern" },
    { "siegel",
      Q_FROM_FRAC(-4, 10),        Q_FROM_FRAC(6, 10),
      Q_FROM_FRAC(0, 1),          Q_FROM_FRAC(0, 1),
      Q_FROM_FRAC(16, 10),        128,
      "Siegel disk, c = -0.4 + 0.6i — rotational invariant disk inside" },
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

/* ── Julia kernel ───────────────────────────────────────────────────── */

/* Iterate z_{n+1} = z_n^2 + c starting from z_0 = (zr, zi), with
 * c = (cr, ci) fixed for the whole frame.  Count iterations until
 * |z|^2 > 4 or max_iter is reached.  Same structure and same three
 * qmul per iter as mandel_iter; the only difference is *what
 * varies across pixels* — here it's z_0, not c. */
static int julia_iter(q_t zr, q_t zi, q_t cr, q_t ci, int max_iter) {
    for (int i = 0; i < max_iter; i++) {
        q_t zr2 = qmul(zr, zr);
        q_t zi2 = qmul(zi, zi);
        if (zr2 + zi2 > Q_ESCAPE) return i;
        q_t zrzi = qmul(zr, zi);
        zr = zr2 - zi2 + cr;
        zi = (zrzi << 1) + ci;
    }
    return max_iter;
}

/* ── Rendering ──────────────────────────────────────────────────────── */
/*
 * The same three renderers mandelbrot has, differing only in which of z
 * and c the pixel supplies: here the pixel is z_0 and c is fixed by the
 * preset, which is what turns one kernel into a family of shapes.
 */

struct julia {
    const struct preset *preset;
    int                  max_iter;
};

static struct julia state = { NULL, -1 };

static int parse_args(int argc, char **argv, void *vs) {
    struct julia *j = vs;
    int i = 0;

    if (i < argc && strcmp(argv[i], "list") == 0) {
        list_presets();
        exit(0);
    }
    if (i < argc && !dterm_looks_like_int(argv[i])) {
        j->preset = find_preset(argv[i]);
        if (j->preset == NULL) {
            fprintf(stderr, "unknown preset '%s' (try `list')\n", argv[i]);
            return -1;
        }
        i++;
    } else {
        j->preset = &presets[0];
    }
    if (i < argc)
        j->max_iter = atoi(argv[i++]);
    if (j->max_iter < 0)
        j->max_iter = j->preset->def_max_iter;
    if (j->max_iter < 4 || j->max_iter > FRACTAL_MAX_ITER) {
        fprintf(stderr, "max_iter must be between 4 and %d\n",
            FRACTAL_MAX_ITER);
        return -1;
    }
    return 0;
}

static void render_mono(const struct demo_surface *s, void *vs) {
    const struct julia *j = vs;
    const struct demo_viewport v = demo_viewport(s, j->preset->view_cx,
        j->preset->view_cy, j->preset->view_hw, s->height);
    char *line = malloc((size_t)s->width + 2);

    if (!line) { perror("malloc"); exit(1); }
    for (unsigned py = 0; py < s->height; py++) {
        q_t zi = v.y_min + (q_t)((int64_t)py * v.dy);
        for (unsigned px = 0; px < s->width; px++) {
            q_t zr = v.x_min + (q_t)((int64_t)px * v.dx);
            int it = julia_iter(zr, zi, j->preset->cr, j->preset->ci,
                j->max_iter);
            int idx = (it >= j->max_iter)
                ? FRACTAL_RAMP_LEN - 1
                : (it * (FRACTAL_RAMP_LEN - 1)) / j->max_iter;
            line[px] = fractal_ramp[idx];
        }
        line[s->width]     = '\n';
        line[s->width + 1] = '\0';
        fputs(line, stdout);
    }
    free(line);
}

static void render_blocks(const struct demo_surface *s, void *vs) {
    const struct julia *j = vs;
    /* Two vertical samples per cell: emit the upper-half-block glyph
     * with fg = top sample, bg = bottom. */
    const unsigned rows = s->height * 2;
    const struct demo_viewport v = demo_viewport(s, j->preset->view_cx,
        j->preset->view_cy, j->preset->view_hw, rows);
    /* Worst case per cell: two SGR sequences plus U+2580 in UTF-8. */
    char *line = malloc((size_t)s->width * 32 + 16);

    if (!line) { perror("malloc"); exit(1); }
    for (unsigned cy = 0; cy < s->height; cy++) {
        q_t zi_top = v.y_min + (q_t)((int64_t)(cy * 2)     * v.dy);
        q_t zi_bot = v.y_min + (q_t)((int64_t)(cy * 2 + 1) * v.dy);
        char *out  = line;
        int last_fg = -2, last_bg = -2;   /* force the first emit */

        for (unsigned px = 0; px < s->width; px++) {
            q_t zr = v.x_min + (q_t)((int64_t)px * v.dx);
            int fg = fractal_cell_color(julia_iter(zr, zi_top,
                j->preset->cr, j->preset->ci, j->max_iter), j->max_iter);
            int bg = fractal_cell_color(julia_iter(zr, zi_bot,
                j->preset->cr, j->preset->ci, j->max_iter), j->max_iter);

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

static void render_pixels(const struct demo_surface *s, void *vs) {
    const struct julia *j = vs;
    const struct demo_viewport v = demo_viewport(s, j->preset->view_cx,
        j->preset->view_cy, j->preset->view_hw, s->height);
    uint8_t r[256], g[256], b[256];

    fractal_build_cmap(r, g, b, s->cmap_entries, j->max_iter);
    demo_set_cmap(s, r, g, b);

    for (unsigned py = 0; py < s->height; py++) {
        q_t zi = v.y_min + (q_t)((int64_t)py * v.dy);
        uint8_t *row = s->pix + (size_t)py * s->stride;

        for (unsigned px = 0; px < s->width; px++) {
            q_t zr = v.x_min + (q_t)((int64_t)px * v.dx);
            row[px] = fractal_pixel_index(julia_iter(zr, zi, j->preset->cr,
                j->preset->ci, j->max_iter));
        }
    }
}

static void render_cells(const struct demo_surface *s, void *vs) {
    if (s->blocks)
        render_blocks(s, vs);
    else
        render_mono(s, vs);
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name         = "julia",
        .render_cell  = render_cells,
        .render_pixel = render_pixels,
        .parse        = parse_args,
        .usage_tail   = "[PRESET] [MAX_ITER]\n       julia list",
        .state        = &state,
    };

    return demo_main(argc, argv, &d);
}

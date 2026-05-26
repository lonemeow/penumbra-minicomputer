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

/* ── Q4.28 fixed-point helpers (mirrored from mandelbrot.c) ─────────── */

typedef int32_t q_t;

#define Q_FRAC_BITS  28
#define Q_ONE        ((q_t)1 << Q_FRAC_BITS)
#define Q_INT(n)     ((q_t)((n) * (int64_t)Q_ONE))

static inline q_t qmul(q_t a, q_t b) {
    return (q_t)(((int64_t)a * (int64_t)b) >> Q_FRAC_BITS);
}

#define Q_ESCAPE     Q_INT(4)
#define Q_FROM_FRAC(num, den) \
    ((q_t)(((int64_t)(num) * (int64_t)Q_ONE) / (den)))

/* ── ANSI 256-color palette ─────────────────────────────────────────── */

/* Same 30-step hue cycle as mandelbrot.c.  See that file for the
 * derivation and swap-out options (fire/ocean/gray). */
static const uint8_t palette[] = {
    196, 202, 208, 214, 220, 226,
    190, 154, 118,  82,  46,
     47,  48,  49,  50,  51,
     45,  39,  33,  27,  21,
     57,  93, 129, 165, 201,
    200, 199, 198, 197,
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))

#define COLOR_INSET   16   /* near-black for "inside the filled Julia set" */

static uint8_t pixel_color(int iter, int max_iter) {
    if (iter >= max_iter) return COLOR_INSET;
    return palette[iter % PALETTE_LEN];
}

enum render_mode {
    MODE_MONO = 0,
    MODE_BLOCKS,
};

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

static const char ramp[] = " .:-=+*#%@";
#define RAMP_LEN  (sizeof(ramp) - 1)

struct viewport {
    q_t x_min, y_min, dx, dy;
};

static struct viewport make_viewport(const struct preset *p,
                                     int width, int cell_height,
                                     int pixel_rows) {
    const q_t half_w = p->view_hw;
    const q_t half_h = (q_t)(((int64_t)half_w * 2 * cell_height) / width);
    struct viewport v = {
        .x_min = p->view_cx - half_w,
        .y_min = p->view_cy - half_h,
        .dx    = (q_t)(((int64_t)(2 * half_w)) / (width      - 1)),
        .dy    = (q_t)(((int64_t)(2 * half_h)) / (pixel_rows - 1)),
    };
    return v;
}

static void render_mono(const struct preset *p, int width, int height,
                        int max_iter) {
    const struct viewport v = make_viewport(p, width, height, height);
    char *line = malloc((size_t)width + 2);
    if (!line) { perror("malloc"); exit(1); }

    for (int py = 0; py < height; py++) {
        q_t zi0 = v.y_min + (q_t)((int64_t)py * v.dy);
        for (int px = 0; px < width; px++) {
            q_t zr0 = v.x_min + (q_t)((int64_t)px * v.dx);
            int it = julia_iter(zr0, zi0, p->cr, p->ci, max_iter);
            int idx = (it >= max_iter) ? (int)(RAMP_LEN - 1)
                                       : (it * (int)(RAMP_LEN - 1)) / max_iter;
            line[px] = ramp[idx];
        }
        line[width]     = '\n';
        line[width + 1] = '\0';
        fputs(line, stdout);
    }
    free(line);
}

static void render_blocks(const struct preset *p, int width, int height,
                          int max_iter) {
    const int pixel_rows = 2 * height;
    const struct viewport v = make_viewport(p, width, height, pixel_rows);
    char *line = malloc((size_t)width * 32 + 16);
    if (!line) { perror("malloc"); exit(1); }

    for (int cy = 0; cy < height; cy++) {
        q_t zi_top = v.y_min + (q_t)((int64_t)(cy * 2)     * v.dy);
        q_t zi_bot = v.y_min + (q_t)((int64_t)(cy * 2 + 1) * v.dy);
        char *out  = line;
        int last_fg = -2, last_bg = -2;

        for (int px = 0; px < width; px++) {
            q_t zr0 = v.x_min + (q_t)((int64_t)px * v.dx);
            int it_top = julia_iter(zr0, zi_top, p->cr, p->ci, max_iter);
            int it_bot = julia_iter(zr0, zi_bot, p->cr, p->ci, max_iter);
            int fg = pixel_color(it_top, max_iter);
            int bg = pixel_color(it_bot, max_iter);

            if (fg != last_fg || bg != last_bg) {
                out += sprintf(out, "\033[38;5;%u;48;5;%um",
                               (unsigned)fg, (unsigned)bg);
                last_fg = fg;
                last_bg = bg;
            }
            *out++ = '\xe2'; *out++ = '\x96'; *out++ = '\x80';
        }
        *out++ = '\033'; *out++ = '['; *out++ = '0'; *out++ = 'm';
        *out++ = '\n';
        *out   = '\0';
        fputs(line, stdout);
    }
    free(line);
}

static void render(const struct preset *p, int width, int height,
                   int max_iter, enum render_mode mode) {
    if (mode == MODE_BLOCKS) render_blocks(p, width, height, max_iter);
    else                     render_mono(p, width, height, max_iter);
}

/* ── Main ───────────────────────────────────────────────────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int looks_like_int(const char *s) {
    return s && s[0] >= '0' && s[0] <= '9';
}

int main(int argc, char **argv) {
    const char *preset_name = "rabbit";    /* most iconic default */
    int width    = 78;
    int height   = 39;
    int max_iter = -1;
    enum render_mode mode = isatty(fileno(stdout)) ? MODE_BLOCKS : MODE_MONO;

    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "--mono") == 0 ||
            strcmp(argv[argi], "-m")     == 0) {
            mode = MODE_MONO; argi++;
        } else if (strcmp(argv[argi], "--blocks") == 0 ||
                   strcmp(argv[argi], "-b")       == 0 ||
                   strcmp(argv[argi], "--color")  == 0 ||
                   strcmp(argv[argi], "-c")       == 0) {
            mode = MODE_BLOCKS; argi++;
        } else {
            break;
        }
    }
    if (argi < argc && strcmp(argv[argi], "list") == 0) {
        list_presets();
        return 0;
    }
    if (argi < argc && !looks_like_int(argv[argi])) {
        preset_name = argv[argi++];
    }
    if (argi < argc) width    = atoi(argv[argi++]);
    if (argi < argc) height   = atoi(argv[argi++]);
    if (argi < argc) max_iter = atoi(argv[argi++]);

    const struct preset *p = find_preset(preset_name);
    if (!p) {
        fprintf(stderr, "%s: unknown preset '%s' "
                        "(try '%s list')\n",
                argv[0], preset_name, argv[0]);
        return 2;
    }
    if (max_iter < 0) max_iter = p->def_max_iter;

    if (width < 8 || height < 4 || max_iter < 4) {
        fprintf(stderr,
                "usage: %s [-b|--blocks | -m|--mono] [PRESET] "
                       "[WIDTH>=8] [HEIGHT>=4] [MAX_ITER>=4]\n"
                "       %s list\n",
                argv[0], argv[0]);
        return 2;
    }

    const int pixel_rows  = (mode == MODE_BLOCKS) ? 2 * height : height;
    const char *mode_desc = (mode == MODE_BLOCKS)
                          ? "half-block + 256-color"
                          : "monochrome ASCII";

    printf("Penumbra Julia: %s (%s)\n", p->name, p->desc);
    printf("  %dx%d cells (%dx%d samples), max_iter=%d, Q4.28, %s\n\n",
           width, height, width, pixel_rows, max_iter, mode_desc);
    fflush(stdout);

    uint64_t t0 = now_ns();
    render(p, width, height, max_iter, mode);
    uint64_t t1 = now_ns();

    uint64_t elapsed_ns  = t1 - t0;
    uint64_t samples     = (uint64_t)width * (uint64_t)pixel_rows;
    uint64_t us_per_samp = (elapsed_ns / 1000ull) / (samples ? samples : 1);

    printf("\nelapsed: %llu.%03llu s  (%llu us/sample, %llu samples)\n",
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           (unsigned long long)us_per_samp,
           (unsigned long long)samples);
    return 0;
}

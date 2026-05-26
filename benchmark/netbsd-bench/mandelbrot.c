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
 * Usage: mandelbrot [-m|--mono | -b|--blocks] [PRESET]
 *                   [WIDTH] [HEIGHT] [MAX_ITER]
 *        mandelbrot list
 *   defaults: full preset, 78 x 39, per-preset max_iter,
 *             blocks mode on TTY / mono mode when piped
 *
 * Render modes:
 *   mono    pure ASCII brightness ramp, no escape codes; works on any
 *           terminal, captures cleanly to a text file
 *   blocks  Unicode U+2580 upper-half-block + ANSI 256-color fg/bg per
 *           cell, doubles effective vertical resolution.  Needs an
 *           xterm-level terminal (most modern emulators qualify).
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

/* ── Q4.28 fixed-point helpers ───────────────────────────────────────── */

typedef int32_t q_t;            /* Q4.28 fixed-point */

#define Q_FRAC_BITS  28
#define Q_ONE        ((q_t)1 << Q_FRAC_BITS)

/* Convert a small integer literal at compile time. */
#define Q_INT(n)     ((q_t)((n) * (int64_t)Q_ONE))

/* Multiply two Q4.28 values; result is Q4.28.  64-bit intermediate is
 * essential — squaring a value near 2.0 in Q4.28 produces a 60+ bit
 * product before the shift. */
static inline q_t qmul(q_t a, q_t b) {
    return (q_t)(((int64_t)a * (int64_t)b) >> Q_FRAC_BITS);
}

/* Escape threshold: |z|^2 > 4, i.e. 4.0 in Q4.28. */
#define Q_ESCAPE     Q_INT(4)

/* Q4.28 from a rational num/den.  Useful for fractional viewport
 * coordinates that can't be written as integer multiples of Q_ONE.
 * We multiply by Q_ONE rather than shift-left, since shifting a
 * negative signed value is implementation-defined. */
#define Q_FROM_FRAC(num, den) \
    ((q_t)(((int64_t)(num) * (int64_t)Q_ONE) / (den)))

/* ── ANSI 256-color palette ──────────────────────────────────────────
 *
 * 30-step hue cycle through xterm's 256-color cube (indices 16..231
 * form a 6x6x6 RGB grid where idx = 16 + 36*r + 6*g + b for r,g,b
 * in [0..5]).  These stops trace
 *   red -> yellow -> green -> cyan -> blue -> magenta -> red.
 *
 * Pairing this with the brightness-ramp character gives a two-channel
 * cell: color hue = iteration band, character density = iteration
 * count.  Both signals stack visibly at typical font sizes.
 *
 * Swap-out options for different aesthetics:
 *   fire:    16, 52, 88, 124, 160, 196, 202, 208, 214, 220, 226
 *   ocean:   16, 17, 18, 19, 20, 21, 27, 33, 39, 45, 51
 *   gray:    232..255 (xterm's 24-step gray ramp) */
static const uint8_t palette[] = {
    196, 202, 208, 214, 220, 226,      /* red    -> yellow  */
    190, 154, 118,  82,  46,           /* yellow -> green   */
     47,  48,  49,  50,  51,           /* green  -> cyan    */
     45,  39,  33,  27,  21,           /* cyan   -> blue    */
     57,  93, 129, 165, 201,           /* blue   -> magenta */
    200, 199, 198, 197,                /* magenta-> red     */
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))

/* Color used for "inside the set" cells in blocks mode (both fg and
 * bg get this on in-set halves).  16 is the true-black corner of the
 * 6x6x6 cube — renders as solid silhouette on light backgrounds and
 * "voids in the rainbow" on dark backgrounds.  Both look good. */
#define COLOR_INSET   16

/* Map iter count to an xterm 256-color index.
 *
 * `iter % PALETTE_LEN` cycles the palette — produces tight color
 * bands that trace the equipotentials around the boundary, which is
 * the visually striking choice for zoomed views.  For a smooth
 * gradient instead, use `palette[(iter * PALETTE_LEN) / max_iter]`. */
static uint8_t pixel_color(int iter, int max_iter) {
    if (iter >= max_iter) return COLOR_INSET;
    return palette[iter % PALETTE_LEN];
}

/* Render mode.  MODE_MONO is portable to any terminal (pure ASCII,
 * no escapes); MODE_BLOCKS uses U+2580 upper-half-block + ANSI
 * 256-color fg+bg to double effective vertical resolution. */
enum render_mode {
    MODE_MONO = 0,
    MODE_BLOCKS,
};

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
static const char ramp[] = " .:-=+*#%@";
#define RAMP_LEN  (sizeof(ramp) - 1)

/* Shared geometry: derives the viewport rectangle from a preset and
 * a target pixel grid.  `cell_height` is always H (drives aspect —
 * physical cell shape is 1 wide x 2 tall regardless of pixel mode).
 * `pixel_rows` is H for mono (one sample per cell) or 2H for blocks
 * (two stacked samples per cell) and drives sample spacing only. */
struct viewport {
    q_t x_min, y_min, dx, dy;
};

static struct viewport make_viewport(const struct preset *p,
                                     int width, int cell_height,
                                     int pixel_rows) {
    /* Aspect: half_h = half_w * 2H/W keeps circles round.  Derived
     * from cell shape (1x2), independent of pixel sampling density. */
    const q_t half_w = p->half_w;
    const q_t half_h = (q_t)(((int64_t)half_w * 2 * cell_height) / width);
    struct viewport v = {
        .x_min = p->cx - half_w,
        .y_min = p->cy - half_h,
        .dx    = (q_t)(((int64_t)(2 * half_w)) / (width      - 1)),
        .dy    = (q_t)(((int64_t)(2 * half_h)) / (pixel_rows - 1)),
    };
    return v;
}

static void render_mono(const struct preset *p, int width, int height,
                        int max_iter) {
    const struct viewport v = make_viewport(p, width, height, height);

    /* line: width cells + newline + NUL */
    char *line = malloc((size_t)width + 2);
    if (!line) { perror("malloc"); exit(1); }

    for (int py = 0; py < height; py++) {
        q_t ci = v.y_min + (q_t)((int64_t)py * v.dy);
        for (int px = 0; px < width; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            int it = mandel_iter(cr, ci, max_iter);
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
    /* 2 vertical pixels per cell — sample at 2H rows, emit H lines
     * of the upper-half-block character with fg = top pixel color,
     * bg = bottom pixel color. */
    const int pixel_rows = 2 * height;
    const struct viewport v = make_viewport(p, width, height, pixel_rows);

    /* Each cell worst case: `\033[38;5;NNN;48;5;MMMm` (22 B) + U+2580
     * in UTF-8 (3 B) = 25 B.  Plus reset + newline.  width*32+16 is
     * comfortable. */
    char *line = malloc((size_t)width * 32 + 16);
    if (!line) { perror("malloc"); exit(1); }

    for (int cy = 0; cy < height; cy++) {
        q_t ci_top = v.y_min + (q_t)((int64_t)(cy * 2)     * v.dy);
        q_t ci_bot = v.y_min + (q_t)((int64_t)(cy * 2 + 1) * v.dy);
        char *out  = line;
        int last_fg = -2, last_bg = -2;   /* force first emit */

        for (int px = 0; px < width; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            int it_top = mandel_iter(cr, ci_top, max_iter);
            int it_bot = mandel_iter(cr, ci_bot, max_iter);
            int fg = pixel_color(it_top, max_iter);
            int bg = pixel_color(it_bot, max_iter);

            /* RLE on the fg/bg pair: flat regions of the same iter
             * band cost one combined escape, not one per cell. */
            if (fg != last_fg || bg != last_bg) {
                out += sprintf(out, "\033[38;5;%u;48;5;%um",
                               (unsigned)fg, (unsigned)bg);
                last_fg = fg;
                last_bg = bg;
            }
            /* U+2580 UPPER HALF BLOCK, UTF-8: E2 96 80 */
            *out++ = '\xe2'; *out++ = '\x96'; *out++ = '\x80';
        }
        /* Reset attributes — so the next row doesn't inherit, and
         * the prompt after the picture starts uncolored. */
        *out++ = '\033'; *out++ = '['; *out++ = '0'; *out++ = 'm';
        *out++ = '\n';
        *out   = '\0';
        fputs(line, stdout);
    }
    free(line);
}

static void render(const struct preset *p, int width, int height,
                   int max_iter, enum render_mode mode) {
    if (mode == MODE_BLOCKS) {
        render_blocks(p, width, height, max_iter);
    } else {
        render_mono(p, width, height, max_iter);
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Detect whether argv[1] is a preset name vs a positional integer.
 * Width is always a positive integer, so any non-digit first character
 * means "this is a preset name". */
static int looks_like_int(const char *s) {
    return s && s[0] >= '0' && s[0] <= '9';
}

int main(int argc, char **argv) {
    const char *preset_name = "full";
    int width    = 78;
    int height   = 39;     /* odd: middle row lands on y=cy (clean spike) */
    int max_iter = -1;     /* -1 => use the preset's own default */
    /* Default: rich (blocks + color) when stdout is a terminal,
     * pure ASCII when piped — standard Unix isatty convention. */
    enum render_mode mode = isatty(fileno(stdout)) ? MODE_BLOCKS : MODE_MONO;

    int argi = 1;
    /* Optional render-mode flags anywhere in the leading args.
     * `--color` is kept as an alias for `--blocks` so existing
     * commands and screenshots stay valid. */
    while (argi < argc) {
        if (strcmp(argv[argi], "--mono") == 0 ||
            strcmp(argv[argi], "-m")     == 0) {
            mode = MODE_MONO;
            argi++;
        } else if (strcmp(argv[argi], "--blocks") == 0 ||
                   strcmp(argv[argi], "-b")       == 0 ||
                   strcmp(argv[argi], "--color")  == 0 ||
                   strcmp(argv[argi], "-c")       == 0) {
            mode = MODE_BLOCKS;
            argi++;
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
                        "(try '%s list' for available presets)\n",
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

    /* In blocks mode each cell holds two stacked pixels, so the
     * sample count (= mandel_iter calls) is W*2H, not W*H. */
    const int pixel_rows  = (mode == MODE_BLOCKS) ? 2 * height : height;
    const char *mode_desc = (mode == MODE_BLOCKS)
                          ? "half-block + 256-color"
                          : "monochrome ASCII";

    printf("Penumbra Mandelbrot: %s (%s)\n", p->name, p->desc);
    printf("  %dx%d cells (%dx%d samples), max_iter=%d, Q4.28, %s\n\n",
           width, height, width, pixel_rows, max_iter, mode_desc);
    fflush(stdout);

    uint64_t t0 = now_ns();
    render(p, width, height, max_iter, mode);
    uint64_t t1 = now_ns();

    uint64_t elapsed_ns  = t1 - t0;
    uint64_t samples     = (uint64_t)width * (uint64_t)pixel_rows;
    /* us/sample = ns/sample / 1000.  At 25 MHz with ~3 software
     * multiplies per iter, expect tens-to-hundreds of microseconds
     * per sample today; hardware MUL should knock this down sharply. */
    uint64_t us_per_samp = (elapsed_ns / 1000ull) / (samples ? samples : 1);

    printf("\nelapsed: %llu.%03llu s  (%llu us/sample, %llu samples)\n",
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           (unsigned long long)us_per_samp,
           (unsigned long long)samples);
    return 0;
}

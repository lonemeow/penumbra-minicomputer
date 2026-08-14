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
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <dev/wscons/wsconsio.h>

#include "perfctr.h"
#include "dterm.h"

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
    MODE_FB,
};

/* ── Framebuffer output ──────────────────────────────────────────────── */
/*
 * The wsdisplay dumb-framebuffer path: ask the device its geometry,
 * install a colormap, switch the screen to the pixel source, and map
 * the pixels straight into this process.  There is no shadow and no
 * blit call — a store to the mapping is a store to the device.
 *
 * The mapping covers the pixel aperture alone; the driver refuses any
 * offset outside it, so a stray write cannot reach the control
 * registers that share the device.
 *
 * The target is stdout — the picture goes where the program's output
 * was already going, so running this from the serial console renders
 * on the serial console's terms and does not reach across to seize a
 * screen nobody asked about.  A device path is the way to say
 * otherwise, and then it is a deliberate act rather than a default.
 */

struct fbdev {
    int      fd;
    int      own_fd;        /* opened here; stdout's is only borrowed */
    uint8_t *pix;
    size_t   size;
    u_int    width;
    u_int    height;
    u_int    stride;
    u_int    cmsize;
    int      mode_set;      /* screen switched, must be put back */
};

static uint8_t *iter_to_cmap_idx;

static void fb_build_cmap(uint8_t *r, uint8_t *g, uint8_t *b, u_int cmsize,
                          int max_iter)
{
    iter_to_cmap_idx = malloc((max_iter + 1) * sizeof(uint8_t));

    for (u_int i = 0; i < cmsize; i++) {
        r[i] = 0x00;
        g[i] = 0x00;
        b[i] = 0x00;
    }

    for (int i = 0; i < max_iter; i++) {
        int cm_idx = (int)(((u_int)i * (cmsize - 1)) / (u_int)max_iter);
        iter_to_cmap_idx[i] = cm_idx;

        double t = (double)cm_idx / 255.0;
        double rv = 9.0  * (1.0 - t) * t * t * t;
        double gv = 15.0 * (1.0 - t) * (1.0 - t) * t * t;
        double bv = 8.5  * (1.0 - t) * (1.0 - t) * (1.0 - t) * t;

        r[cm_idx] = (uint8_t)(rv * 255.0);
        g[cm_idx] = (uint8_t)(gv * 255.0);
        b[cm_idx] = (uint8_t)(bv * 255.0);
    }

    // Non-escaping
    iter_to_cmap_idx[max_iter] = cmsize - 1;
    r[cmsize - 1] = 0x00;
    g[cmsize - 1] = 0x00;
    b[cmsize - 1] = 0x00;
}

static uint8_t fb_palette_index(int iter, int max_iter)
{
    return iter_to_cmap_idx[iter];
}

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

    /* Worst case per cell: two SGR sequences plus U+2580 in UTF-8. */
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
                out = dterm_pair_color(out, fg, bg);
                last_fg = fg;
                last_bg = bg;
            }
            out = dterm_pair_glyph(out);
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

/* ── Framebuffer plumbing ────────────────────────────────────────────── */

static int fb_open(struct fbdev *fb, const char *path, int max_iter) {
    struct wsdisplayio_fbinfo fbi;
    struct wsdisplay_cmap cm;
    uint8_t r[256], g[256], b[256];
    const char *what = path ? path : "stdout";
    u_int mode;

    memset(fb, 0, sizeof(*fb));
    if (path == NULL) {
        fb->fd = fileno(stdout);
    } else {
        /* O_NOCTTY: this is a display device here, not a terminal to
         * talk on.  Run from a shell the process already has a
         * controlling tty and nothing would happen, but run without
         * one — from a script, or at boot — opening a tty would
         * otherwise acquire it, and the screen about to be taken for
         * pixels would become the process's terminal. */
        fb->fd = open(path, O_RDWR | O_NOCTTY);
        if (fb->fd < 0) {
            fprintf(stderr, "mandelbrot: %s: %s\n", path, strerror(errno));
            return -1;
        }
        fb->own_fd = 1;
    }

    if (ioctl(fb->fd, WSDISPLAYIO_GET_FBINFO, &fbi) != 0) {
        fprintf(stderr, "mandelbrot: %s has no framebuffer: %s\n",
                what, strerror(errno));
        if (fb->own_fd) close(fb->fd);
        return -1;
    }
    if (fbi.fbi_bitsperpixel != 8 || fbi.fbi_pixeltype != WSFB_CI) {
        fprintf(stderr, "mandelbrot: unsupported format "
                "(%u bpp, pixeltype %u)\n",
                fbi.fbi_bitsperpixel, fbi.fbi_pixeltype);
        if (fb->own_fd) close(fb->fd);
        return -1;
    }

    fb->width = fbi.fbi_width;
    fb->height = fbi.fbi_height;
    fb->stride = fbi.fbi_stride;
    fb->size = (size_t)fbi.fbi_fbsize;
    fb->cmsize = fbi.fbi_subtype.fbi_cmapinfo.cmap_entries;
    if (fb->cmsize > 256)
        fb->cmsize = 256;

    fb_build_cmap(r, g, b, fb->cmsize, max_iter);
    cm.index = 0;
    cm.count = fb->cmsize;
    cm.red = r;
    cm.green = g;
    cm.blue = b;
    if (ioctl(fb->fd, WSDISPLAYIO_PUTCMAP, &cm) != 0) {
        fprintf(stderr, "mandelbrot: PUTCMAP: %s\n", strerror(errno));
        if (fb->own_fd) close(fb->fd);
        return -1;
    }

    /* Switch the screen before mapping: the aperture is live either
     * way, but drawing into a picture nobody is looking at would show
     * the render as a jump rather than as it happens. */
    mode = WSDISPLAYIO_MODE_DUMBFB;
    if (ioctl(fb->fd, WSDISPLAYIO_SMODE, &mode) != 0) {
        fprintf(stderr, "mandelbrot: SMODE: %s\n", strerror(errno));
        if (fb->own_fd) close(fb->fd);
        return -1;
    }
    fb->mode_set = 1;

    fb->pix = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fb->fd, 0);
    if (fb->pix == MAP_FAILED) {
        fprintf(stderr, "mandelbrot: mmap: %s\n", strerror(errno));
        mode = WSDISPLAYIO_MODE_EMUL;
        ioctl(fb->fd, WSDISPLAYIO_SMODE, &mode);
        if (fb->own_fd) close(fb->fd);
        return -1;
    }

    /* Pixel contents are undefined until something writes them, and
     * the screen is already showing this source.  A render takes long
     * enough that without a clear the viewer watches the picture eat
     * its way through whatever was in the memory — on a cold boot,
     * noise.  Entry 0 is the first gradient stop, which is black. */
    memset(fb->pix, 0, fb->size);
    return 0;
}

/*
 * Hold the finished picture.  Graphics mode ends when this returns, so
 * without it the render would appear and vanish in the same instant —
 * the console comes back the moment the program is done.
 *
 * A keypress always dismisses.  hold_seconds > 0 also gives up after
 * that long, which is what an unattended exhibit wants; 0 waits for
 * someone, which is what a person at a prompt wants.
 */
static void fb_hold(int hold_seconds) {
    struct termios saved, raw;
    struct timeval tv, *tvp = NULL;
    fd_set rfds;
    unsigned char c;
    int have_tty;

    /* Single keypress, not a line: the reader is a person looking at a
     * picture, not a shell waiting for a command. */
    have_tty = (tcgetattr(STDIN_FILENO, &saved) == 0);
    if (have_tty) {
        raw = saved;
        raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    if (hold_seconds > 0) {
        tv.tv_sec = hold_seconds;
        tv.tv_usec = 0;
        tvp = &tv;
    }
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, tvp) > 0)
        (void)read(STDIN_FILENO, &c, 1);

    if (have_tty)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
}

/* Put the character console back.  wsdisplay also does this when the
 * last close happens, so a crash recovers too; doing it here is what
 * makes a normal exit land on a readable screen rather than the last
 * frame. */
static void fb_close(struct fbdev *fb) {
    u_int mode = WSDISPLAYIO_MODE_EMUL;

    if (fb->pix != NULL && fb->pix != MAP_FAILED)
        munmap(fb->pix, fb->size);
    if (fb->mode_set)
        ioctl(fb->fd, WSDISPLAYIO_SMODE, &mode);
    if (fb->own_fd)
        close(fb->fd);
}

/*
 * One sample per pixel, written straight into the mapping.
 *
 * The viewport helper takes cell height and sample rows separately
 * because the terminal's cells are twice as tall as they are wide.
 * Framebuffer pixels are square — the device doubles both axes
 * equally — so passing half the row count as the cell height cancels
 * the helper's 2x and keeps circles round.
 */
static void render_fb(const struct preset *p, struct fbdev *fb,
                      int max_iter) {
    const int w = (int)fb->width, h = (int)fb->height;
    const struct viewport v = make_viewport(p, w, h / 2, h);

    for (int py = 0; py < h; py++) {
        q_t ci = v.y_min + (q_t)((int64_t)py * v.dy);
        uint8_t *row = fb->pix + (size_t)py * fb->stride;

        for (int px = 0; px < w; px++) {
            q_t cr = v.x_min + (q_t)((int64_t)px * v.dx);
            row[px] = fb_palette_index(mandel_iter(cr, ci, max_iter),
                                       max_iter);
        }
    }
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

int main(int argc, char **argv) {
    const char *preset_name = "full";
    int width    = 78;
    int height   = 39;     /* odd: middle row lands on y=cy (clean spike) */
    int max_iter = -1;     /* -1 => use the preset's own default */
    const char *fb_path = NULL;   /* NULL => stdout's device */
    int hold_seconds = 0;         /* 0 => hold until a keypress */
    enum render_mode mode;
    struct fbdev fb;

    /* Seven rows go to the header, the blanks around the picture, the
     * timing line and the three-line counter report at exit. */
    dterm_init(&width, &height, 2, 5);

    /* Colour when something is watching, ASCII when piped. */
    mode = isatty(fileno(stdout)) ? MODE_BLOCKS : MODE_MONO;

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
        } else if ((strcmp(argv[argi], "--hold") == 0 ||
                    strcmp(argv[argi], "-H")     == 0) &&
                   argi + 1 < argc) {
            hold_seconds = atoi(argv[argi + 1]);
            argi += 2;
        } else if (strcmp(argv[argi], "--fb") == 0 ||
                   strcmp(argv[argi], "-f")   == 0) {
            mode = MODE_FB;
            argi++;
            /* An optional device path sends the picture somewhere
             * other than where the program's output already goes. */
            if (argi < argc && argv[argi][0] == '/')
                fb_path = argv[argi++];
        } else {
            break;
        }
    }
    if (argi < argc && strcmp(argv[argi], "list") == 0) {
        list_presets();
        return 0;
    }
    if (argi < argc && !dterm_looks_like_int(argv[argi])) {
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
                "usage: %s [-b|--blocks | -m|--mono | -f|--fb [DEV]] "
                       "[-H|--hold SECONDS] "
                       "[PRESET] [WIDTH>=8] [HEIGHT>=4] [MAX_ITER>=4]\n"
                "       %s list\n",
                argv[0], argv[0]);
        return 2;
    }

    /* The framebuffer's geometry comes from the device, so WIDTH and
     * HEIGHT do not apply; in blocks mode each cell holds two stacked
     * pixels, so the sample count is W*2H rather than W*H. */
    if (mode == MODE_FB && fb_open(&fb, fb_path, max_iter) != 0)
        return 1;

    const int pixel_rows = (mode == MODE_FB)     ? (int)fb.height :
                           (mode == MODE_BLOCKS) ? 2 * height : height;
    const int pixel_cols = (mode == MODE_FB) ? (int)fb.width : width;

    if (mode == MODE_FB) {
        printf("Mandelbrot %s  %ux%u pixels on %s  iter=%d  Q4.28\n\n",
               p->name, fb.width, fb.height,
               fb_path ? fb_path : "this terminal", max_iter);
    } else {
        printf("Mandelbrot %s  %dx%d cells (%dx%d samples)  iter=%d  "
               "Q4.28  %s\n\n",
               p->name, width, height, width, pixel_rows, max_iter,
               dterm_mode_name(mode == MODE_BLOCKS));
    }
    fflush(stdout);

    uint64_t t0 = now_ns();
    perf_demo_track();          /* snapshot perfctrs; dump breakdown at exit */
    if (mode == MODE_FB)
        render_fb(p, &fb, max_iter);
    else
        render(p, width, height, max_iter, mode);
    uint64_t t1 = now_ns();

    uint64_t elapsed_ns  = t1 - t0;
    uint64_t samples     = (uint64_t)pixel_cols * (uint64_t)pixel_rows;
    /* us/sample = ns/sample / 1000.  At 25 MHz with ~3 software
     * multiplies per iter, expect tens-to-hundreds of microseconds
     * per sample today; hardware MUL should knock this down sharply. */
    uint64_t us_per_samp = (elapsed_ns / 1000ull) / (samples ? samples : 1);

    printf("\nelapsed: %llu.%03llu s  (%llu us/sample, %llu samples)\n",
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           (unsigned long long)us_per_samp,
           (unsigned long long)samples);

    /* Hold the picture, then put the console back.  The timing above
     * has already been written; if stdout is the display it was
     * written to cells the graphics mode is covering, so it appears
     * once the console returns. */
    if (mode == MODE_FB) {
        fflush(stdout);
        fb_hold(hold_seconds);
        fb_close(&fb);
    }
    return 0;
}

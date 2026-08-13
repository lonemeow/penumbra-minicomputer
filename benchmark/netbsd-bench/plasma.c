/* plasma.c - demoscene plasma effect + UART throughput measurement.
 *
 * Sums several phase-shifted sin waves into a per-pixel intensity,
 * advancing the phase each frame so the field flows — the classic
 * "computer art from 1992" look.  No multiplies on the hot path
 * (just sin-table lookups), so it's cheap on CPU, but a full-screen
 * redraw effect: per-frame byte cost is large (tens of KB at
 * 78x39 half-block + 256-color), which makes it a poor fit for the
 * 115200-baud serial console — wire transmit time dominates wall
 * clock regardless of how fast the CPU runs.
 *
 * As a result this is not part of the live demo rotation; that slot
 * is filled by demos with smaller per-frame byte cost (Lorenz, etc.).
 * Plasma's value now is as a measurement testbed for the write path:
 * --nodraw separates compute from I/O time, and the bytes/frame stat
 * lets you predict UART transmit time at any baud rate.  Useful when
 * characterizing future I/O optimizations (write() vs fputs, faster
 * UART, lower-bandwidth color schemes).
 *
 * Usage: plasma [-m|--mono | -b|--blocks] [PALETTE]
 *               [FRAMES] [WIDTH] [HEIGHT]
 *        plasma list
 *   defaults: rainbow palette, infinite frames, 78 x 39,
 *             blocks mode on TTY / mono mode when piped
 *
 *   FRAMES = 0 (or omitted) means run until SIGINT.  Setting an
 *   explicit positive count is useful for piping to a file or for
 *   timed benchmark runs.
 *
 * Cursor is hidden while running and restored on exit (whether by
 * SIGINT or by exhausting FRAMES).  Each frame overwrites the
 * previous via cursor-home, so there's no flicker even on slow
 * terminals.
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
#include <termios.h>     /* non-canonical stdin for exit-on-keypress */

#include "perfctr.h"
#include "dterm.h"


/* ── Sin lookup table ────────────────────────────────────────────────
 *
 * 256 entries of sin(2π·i/256), scaled to int8_t range [-127, 127].
 * Index with `(phase & 0xff)` — the 8-bit mask gives free wraparound
 * so phases never overflow into bounds-check territory.
 *
 * Built at startup from libm sin().  Softfloat startup cost is one
 * sweep through 256 doubles; amortizes to nothing across the run. */
static int8_t sin_lut[256];

static void init_sin_lut(void) {
    for (int i = 0; i < 256; i++) {
        double theta = 2.0 * M_PI * (double)i / 256.0;
        sin_lut[i] = (int8_t)lround(sin(theta) * 127.0);
    }
}

/* ── Palettes ────────────────────────────────────────────────────────
 *
 * Each palette is a smoothly-varying ramp of xterm 256-color indices.
 * The plasma field's intensity maps into the table; cycling the
 * palette index by intensity gives the colored bands.  We provide
 * four flavors to match different demo aesthetics. */

static const uint8_t pal_rainbow[] = {
    196, 202, 208, 214, 220, 226,
    190, 154, 118,  82,  46,
     47,  48,  49,  50,  51,
     45,  39,  33,  27,  21,
     57,  93, 129, 165, 201,
    200, 199, 198, 197,
};

static const uint8_t pal_fire[] = {
     16,  52,  88, 124, 160, 196,
    202, 208, 214, 220, 226,
    227, 228, 229, 230, 231,
    230, 229, 228, 227, 226,
    220, 214, 208, 202, 196,
    160, 124,  88,  52,
};

static const uint8_t pal_ocean[] = {
     17,  18,  19,  20,  21,
     27,  33,  39,  45,  51,
     87,  86,  85,  84,  83,
     82,  46,  47,  48,  49,
     50,  51,  45,  39,  33,
     27,  21,  20,  19,  18,
};

static const uint8_t pal_gray[] = {
    232, 233, 234, 235, 236, 237,
    238, 239, 240, 241, 242, 243,
    244, 245, 246, 247, 248, 249,
    250, 251, 252, 253, 254, 255,
    254, 253, 252, 251, 250, 249,
};

struct palette {
    const char    *name;
    const uint8_t *table;
    size_t         len;
    const char    *desc;
};

static const struct palette palettes[] = {
    { "rainbow", pal_rainbow, sizeof(pal_rainbow), "hue cycle through the 6x6x6 cube"        },
    { "fire",    pal_fire,    sizeof(pal_fire),    "black -> red -> orange -> yellow -> white" },
    { "ocean",   pal_ocean,   sizeof(pal_ocean),   "deep blue -> cyan -> light blue"          },
    { "gray",    pal_gray,    sizeof(pal_gray),    "24-step xterm gray ramp"                  },
};
#define NUM_PALETTES  (sizeof(palettes) / sizeof(palettes[0]))

static const struct palette *find_palette(const char *name) {
    for (size_t i = 0; i < NUM_PALETTES; i++) {
        if (strcmp(palettes[i].name, name) == 0) return &palettes[i];
    }
    return NULL;
}

static void list_palettes(void) {
    printf("Palettes:\n");
    for (size_t i = 0; i < NUM_PALETTES; i++) {
        printf("  %-10s  %s\n", palettes[i].name, palettes[i].desc);
    }
}

enum render_mode {
    MODE_MONO = 0,
    MODE_BLOCKS,
};

/* ── Plasma field ────────────────────────────────────────────────────
 *
 * For pixel (x, y) at time t, return an integer "intensity" — any
 * range is fine, the caller normalizes to a palette index.  This is
 * the heart of the demo's *visual character*: the choice of which
 * sin terms to sum (and how their phases couple) determines whether
 * the field looks like rolling bands, organic blobs, plaid weaves,
 * or kaleidoscopic radial ripples. */
static int plasma_field(int x, int y, int t) {
    return sin_lut[(x + (x >> 1) + t) & 0xff]
        + sin_lut[(y * 2 + (y >> 1) + (t >> 1)) & 0xff]
        + sin_lut[((x + y) * 2 + (t >> 2)) & 0xff];
}

/* ── Measurement instrumentation ────────────────────────────────────── */

/* When set, renderers compute the frame and build the line buffer
 * exactly as normal, but skip the final fputs() that pushes bytes to
 * stdio (and hence to the kernel and the UART).  This isolates pure
 * compute time from the full write path. */
static int g_stop;
static int g_do_write = 1;

/* Running total of bytes the renderers *would have* written.  Counted
 * regardless of g_do_write, so the nodraw run still reports the same
 * frame-size number — which is exactly what's needed to predict UART
 * transmit time at any given baud rate. */
static uint64_t g_bytes_rendered = 0;

/* ── Rendering ──────────────────────────────────────────────────────── */

static const char ramp[] = " .:-=+*#%@";
#define RAMP_LEN  (sizeof(ramp) - 1)

/* Map a plasma field value to a palette index by mod-wrap.  Negative
 * values are handled by adding a big multiple of len before mod, so
 * the result is always in [0, len). */
static int field_to_idx(int v, size_t len) {
    int m = v % (int)len;
    if (m < 0) m += (int)len;
    return m;
}

/* Same wrap, but onto the ASCII brightness ramp. */
static int field_to_ramp(int v) {
    return field_to_idx(v, RAMP_LEN);
}

static void render_mono(int width, int height, int t) {
    char *line = malloc((size_t)width + 2);
    if (!line) { perror("malloc"); exit(1); }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int v = plasma_field(x, y, t);
            line[x] = ramp[field_to_ramp(v)];
        }
        line[width]     = '\n';
        line[width + 1] = '\0';
        g_bytes_rendered += (uint64_t)(width + 1);   /* chars + '\n' */
        if (g_do_write) fputs(line, stdout);
    }
    free(line);
}

static void render_blocks(int width, int height, int t,
                          const struct palette *pal) {
    char *line = malloc((size_t)width * 32 + 16);
    if (!line) { perror("malloc"); exit(1); }

    for (int cy = 0; cy < height; cy++) {
        char *out  = line;
        int last_fg = -2, last_bg = -2;
        for (int x = 0; x < width; x++) {
            int v_top = plasma_field(x, cy * 2,     t);
            int v_bot = plasma_field(x, cy * 2 + 1, t);
            int fg = pal->table[field_to_idx(v_top, pal->len)];
            int bg = pal->table[field_to_idx(v_bot, pal->len)];
            if (fg != last_fg || bg != last_bg) {
                out = dterm_pair_color(out, fg, bg);
                last_fg = fg;
                last_bg = bg;
            }
            out = dterm_pair_glyph(out);
        }
        *out++ = '\033'; *out++ = '['; *out++ = '0'; *out++ = 'm';
        *out++ = '\n';
        *out   = '\0';
        g_bytes_rendered += (uint64_t)(out - line);
        if (g_do_write) fputs(line, stdout);
    }
    free(line);
}

/* ── Terminal control + frame loop ──────────────────────────────────── */





static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *palette_name = "rainbow";
    int width    = 78;
    int height   = 39;
    int frames   = 0;        /* 0 = run forever until SIGINT */
    enum render_mode mode;

    /* The picture is redrawn in place, so only the banner and its hint
     * are kept back; the summary at exit may scroll. */
    dterm_init(&width, &height, 2, 0);

    /* Colour when something is watching, ASCII when piped. */
    mode = isatty(fileno(stdout)) ? MODE_BLOCKS : MODE_MONO;

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
        } else if (strcmp(argv[argi], "--nodraw") == 0 ||
                   strcmp(argv[argi], "-n")       == 0) {
            g_do_write = 0; argi++;
        } else {
            break;
        }
    }
    if (argi < argc && strcmp(argv[argi], "list") == 0) {
        list_palettes();
        return 0;
    }
    if (argi < argc && !dterm_looks_like_int(argv[argi])) {
        palette_name = argv[argi++];
    }
    if (argi < argc) frames = atoi(argv[argi++]);
    if (argi < argc) width  = atoi(argv[argi++]);
    if (argi < argc) height = atoi(argv[argi++]);

    const struct palette *pal = find_palette(palette_name);
    if (!pal) {
        fprintf(stderr, "%s: unknown palette '%s' "
                        "(try '%s list')\n",
                argv[0], palette_name, argv[0]);
        return 2;
    }
    if (width < 8 || height < 4 || frames < 0) {
        fprintf(stderr,
                "usage: %s [-b|--blocks | -m|--mono] [PALETTE] "
                       "[FRAMES] [WIDTH>=8] [HEIGHT>=4]\n"
                "       %s list\n",
                argv[0], argv[0]);
        return 2;
    }

    /* One-time cost, amortized across the whole run. */
    init_sin_lut();

    dterm_catch_interrupt();

    /* Put stdin into non-canonical, no-echo mode with VMIN=VTIME=0 so
     * a per-frame read() returns immediately with whatever's queued
     * (or zero bytes if none).  This is "press any key to exit"
     * semantics; SIGINT (Ctrl-C) still works in parallel. */
    int kbd_active = isatty(STDIN_FILENO);
    struct termios orig_tio;
    if (kbd_active) {
        if (tcgetattr(STDIN_FILENO, &orig_tio) == 0) {
            struct termios raw = orig_tio;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        } else {
            kbd_active = 0;  /* not actually a tty; skip teardown too */
        }
    }

    /* Banner before we start clobbering the screen.  Once we go
     * cursor-home, this scrolls off and isn't visible mid-demo —
     * which is the right behavior: a clean canvas. */
    printf("Penumbra Plasma: palette=%s, %dx%d, mode=%s, frames=%s%s\n",
           pal->name, width, height,
           dterm_mode_name(mode == MODE_BLOCKS),
           frames == 0 ? "infinite" : "limited",
           g_do_write ? "" : ", NODRAW (compute only)");
    if (kbd_active) {
        printf("  (press any key or Ctrl-C to stop)\n");
    }
    fflush(stdout);

    if (g_do_write) {
        dterm_cursor(0);
        dterm_clear();
    }

    uint64_t t_start = now_ns();
    perf_demo_track();          /* snapshot perfctrs; dump breakdown at exit */
    int t = 0;
    uint64_t rendered = 0;
    while (!g_stop && !dterm_interrupted() && (frames == 0 || (int)rendered < frames)) {
        if (g_do_write) dterm_home();
        if (mode == MODE_BLOCKS) render_blocks(width, height, t, pal);
        else                     render_mono(width, height, t);
        if (g_do_write) fflush(stdout);
        t++;
        rendered++;

        /* Non-blocking stdin check: with VMIN=0/VTIME=0, read returns
         * >0 only if a byte is actually queued.  EAGAIN/0 are both
         * "nothing to read" and we just keep rendering. */
        if (kbd_active) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) g_stop = 1;
        }
    }
    uint64_t t_end = now_ns();

    /* Restore terminal state BEFORE printing stats — otherwise the
     * stats land at whatever cursor position the last frame left us
     * in, which would be the middle of the picture.  In nodraw mode
     * we never touched the screen, so no restore is needed there. */
    if (g_do_write) {
        dterm_end();
        dterm_cursor(1);
        fflush(stdout);
    }
    if (kbd_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
    }
    putchar('\n');

    /* Stats.  Integer math throughout — no %f, no libm formatting,
     * so the static binary doesn't pick up extra printf bloat.  FPS
     * is reported as fps*100 so we get two decimal places. */
    uint64_t elapsed_ns       = t_end - t_start;
    uint64_t elapsed_ms       = elapsed_ns / 1000000ull;
    uint64_t pixel_rows       = (mode == MODE_BLOCKS) ? 2ull * (uint64_t)height
                                                      : (uint64_t)height;
    uint64_t samples_per_fr   = (uint64_t)width * pixel_rows;
    uint64_t total_samples    = samples_per_fr * rendered;
    uint64_t fps_x100         = (elapsed_ms > 0)
                              ? (rendered * 100000ull) / elapsed_ms
                              : 0;
    uint64_t us_per_frame     = (rendered > 0)
                              ? (elapsed_ns / 1000ull) / rendered
                              : 0;
    uint64_t us_per_sample    = (total_samples > 0)
                              ? (elapsed_ns / 1000ull) / total_samples
                              : 0;

    uint64_t bytes_per_frame  = (rendered > 0)
                              ? g_bytes_rendered / rendered
                              : 0;
    /* At 115200 8N1 baud, each byte = 10 bits on the wire = ~86.8 us. */
    uint64_t uart_us_per_frame = (bytes_per_frame * 10ull * 1000000ull) / 115200ull;

    printf("plasma: %llu frames in %llu.%03llu s%s\n",
           (unsigned long long)rendered,
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           g_do_write ? "" : " (NODRAW)");
    printf("  %llu.%02llu FPS, %llu us/frame, %llu us/sample\n",
           (unsigned long long)(fps_x100 / 100),
           (unsigned long long)(fps_x100 % 100),
           (unsigned long long)us_per_frame,
           (unsigned long long)us_per_sample);
    printf("  geometry: %d x %d cells, %llu samples/frame, %llu total samples\n",
           width, height,
           (unsigned long long)samples_per_fr,
           (unsigned long long)total_samples);
    printf("  output:   %llu bytes/frame, %llu total bytes "
           "(%llu us/frame at 115200 baud)\n",
           (unsigned long long)bytes_per_frame,
           (unsigned long long)g_bytes_rendered,
           (unsigned long long)uart_us_per_frame);
    return 0;
}

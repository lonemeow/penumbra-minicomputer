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

#include "perfctr.h"

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

struct dec_str {
    char    bytes[3];
    uint8_t len;
};
static struct dec_str dec[256];

static void init_dec(void) {
    for (int v = 0; v < 256; v++) {
        if (v >= 100) {
            dec[v].bytes[0] = (char)('0' + v / 100);
            dec[v].bytes[1] = (char)('0' + (v / 10) % 10);
            dec[v].bytes[2] = (char)('0' + v % 10);
            dec[v].len = 3;
        } else if (v >= 10) {
            dec[v].bytes[0] = (char)('0' + v / 10);
            dec[v].bytes[1] = (char)('0' + v % 10);
            dec[v].len = 2;
        } else {
            dec[v].bytes[0] = (char)('0' + v);
            dec[v].len = 1;
        }
    }
}

static inline char *emit_dec(char *out, int n) {
    const struct dec_str *d = &dec[n];
    *out++ = d->bytes[0];
    if (d->len >= 2) *out++ = d->bytes[1];
    if (d->len >= 3) *out++ = d->bytes[2];
    return out;
}

/* ── Measurement instrumentation ────────────────────────────────────── */

static int g_do_write = 1;
static uint64_t g_bytes_rendered = 0;

static inline void emit_bytes(const char *buf, size_t n) {
    g_bytes_rendered += n;
    if (g_do_write) (void)write(STDOUT_FILENO, buf, n);
}

/* ── Intensity buffer + cell emit ───────────────────────────────────── */

/* intensity[col * pixel_rows + row] — saturating uint8_t per half-pixel.
 * Cells get re-emitted whenever a bob splats into them this frame. */
static uint8_t *intensity;
static int g_width;
static int g_pixel_rows;

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
    mark_dirty(col, row);
}

/* 3x3 splat with center-heavy weights.  Tuned so a single visit adds
 * a soft Gaussian-ish bump; many visits accumulate into a saturated
 * core surrounded by a softer halo. */
static const int splat_weights[3][3] = {
    {2,  3, 2},
    {3, 12, 3},
    {2,  3, 2},
};

static void splat(int x, int y) {
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            splat_pixel(x + dx, y + dy, splat_weights[dy + 1][dx + 1]);
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
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, cell_row + 1);
    *out++ = ';';
    out = emit_dec(out, col + 1);
    *out++ = 'H';

    /* \033[38;5;F;48;5;Bm */
    *out++ = '\033'; *out++ = '[';
    *out++ = '3'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, fg);
    *out++ = ';';
    *out++ = '4'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, bg);
    *out++ = 'm';

    /* U+2580 UPPER HALF BLOCK */
    *out++ = '\xe2'; *out++ = '\x96'; *out++ = '\x80';

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
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, cell_row + 1);
    *out++ = ';';
    out = emit_dec(out, col + 1);
    *out++ = 'H';
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
    uint8_t fx, fy;            /* frequency multipliers on time   */
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

    /* Co-prime freq ratios keep the bobs from ever synchronizing.
     * Phases spread around the circle so bobs don't start bunched. */
    bobs[0] = (struct bob){ sax, say, cx - ox, cy - oy,  1, 2,   0,   0 };
    bobs[1] = (struct bob){ sax, say, cx + ox, cy - oy,  3, 2,  50,  30 };
    bobs[2] = (struct bob){ sax, say, cx - ox, cy + oy,  3, 5, 100,  70 };
    bobs[3] = (struct bob){ sax, say, cx + ox, cy + oy,  5, 3, 150, 110 };
    bobs[4] = (struct bob){ sax, say, cx,      cy,       1, 4, 200, 150 };
}

static inline void bob_position(const struct bob *b, int t,
                                int *out_x, int *out_y) {
    /* sin_lut values in [-127, 127]; (amp * sin) >> 7 keeps the
     * result in [-amp, amp] without needing a divide.  Phase + freq*t
     * is naturally masked to 8 bits by the array indexing. */
    int sx = sin_lut[(uint8_t)(b->phx + b->fx * t)];
    int sy = sin_lut[(uint8_t)(b->phy + b->fy * t)];
    *out_x = b->cx + ((b->amp_x * sx) >> 7);
    *out_y = b->cy + ((b->amp_y * sy) >> 7);
}

/* ── Terminal control + signal handling ─────────────────────────────── */

enum render_mode {
    MODE_MONO = 0,
    MODE_BLOCKS,
};

static volatile sig_atomic_t stop_flag = 0;

static void sigint_handler(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void cursor_hide(void)   { fputs("\033[?25l", stdout); }
static void cursor_show(void)   { fputs("\033[?25h", stdout); }
static void clear_screen(void)  { fputs("\033[2J\033[H", stdout); }
static void reset_sgr(void)     { fputs("\033[0m", stdout); }

static int looks_like_int(const char *s) {
    return s && s[0] >= '0' && s[0] <= '9';
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int width    = 78;
    int height   = 39;
    int frames   = 0;
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
        } else if (strcmp(argv[argi], "--nodraw") == 0 ||
                   strcmp(argv[argi], "-n")       == 0) {
            g_do_write = 0; argi++;
        } else {
            break;
        }
    }
    if (argi < argc && !looks_like_int(argv[argi])) {
        fprintf(stderr, "%s: unexpected arg '%s'\n", argv[0], argv[argi]);
        return 2;
    }
    if (argi < argc) frames = atoi(argv[argi++]);
    if (argi < argc) width  = atoi(argv[argi++]);
    if (argi < argc) height = atoi(argv[argi++]);

    if (width < 8 || height < 4 || frames < 0) {
        fprintf(stderr,
                "usage: %s [-b|--blocks | -m|--mono] [-n|--nodraw] "
                       "[FRAMES] [WIDTH>=8] [HEIGHT>=4]\n",
                argv[0]);
        return 2;
    }

    init_sin_lut();
    init_dec();

    g_width      = width;
    g_pixel_rows = (mode == MODE_BLOCKS) ? 2 * height : height;

    /* Intensity buffer: one byte per half-pixel (or per cell in mono). */
    intensity = calloc((size_t)g_width * (size_t)g_pixel_rows, 1);
    /* Dirty-cell list: bounded by bobs * splat-cells-per-bob with a
     * generous margin for overlap (each splat is 3x3 pixels which is
     * at most 3 cols x 3 cell rows = 9 cells in the worst case). */
    dirty_capacity = NUM_BOBS * 12;
    dirty = malloc(sizeof(*dirty) * (size_t)dirty_capacity);
    dirty_flag = calloc((size_t)g_width * (size_t)height, 1);
    if (!intensity || !dirty || !dirty_flag) { perror("malloc"); return 1; }

    init_bobs(width, g_pixel_rows);

    struct sigaction sa = { .sa_handler = sigint_handler };
    sigaction(SIGINT, &sa, NULL);

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
            kbd_active = 0;
        }
    }

    printf("Penumbra Shadebobs: %d bobs, %dx%d cells (%dx%d samples), %s%s\n",
           NUM_BOBS, width, height, width, g_pixel_rows,
           mode == MODE_BLOCKS ? "half-block + 256-color" : "mono ASCII",
           g_do_write ? "" : ", NODRAW");
    if (kbd_active) {
        printf("  (press any key or Ctrl-C to stop)\n");
    }
    fflush(stdout);

    if (g_do_write) {
        cursor_hide();
        clear_screen();
        fflush(stdout);
    }

    uint64_t t_start = now_ns();
    perf_demo_track();          /* snapshot perfctrs; dump breakdown at exit */
    int t = 0;
    uint64_t rendered = 0;
    while (!stop_flag && (frames == 0 || (int)rendered < frames)) {
        /* Reset dirty tracking for this frame. */
        for (int i = 0; i < dirty_count; i++) {
            int cell_idx = dirty[i].col * height + dirty[i].row;
            dirty_flag[cell_idx] = 0;
        }
        dirty_count = 0;

        /* Move and splat each bob. */
        for (int i = 0; i < NUM_BOBS; i++) {
            int bx, by;
            bob_position(&bobs[i], t, &bx, &by);
            splat(bx, by);
        }

        /* Re-emit only the dirty cells. */
        for (int i = 0; i < dirty_count; i++) {
            int col = dirty[i].col;
            int row = dirty[i].row;
            if (mode == MODE_BLOCKS) emit_cell_blocks(col, row);
            else                     emit_cell_mono(col, row);
        }

        if (g_do_write) fflush(stdout);
        t++;
        rendered++;

        if (kbd_active) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) stop_flag = 1;
        }
    }
    uint64_t t_end = now_ns();

    if (g_do_write) {
        reset_sgr();
        cursor_show();
        fflush(stdout);
    }
    if (kbd_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
    }
    putchar('\n');

    uint64_t elapsed_ns      = t_end - t_start;
    uint64_t elapsed_ms      = elapsed_ns / 1000000ull;
    uint64_t fps_x100        = (elapsed_ms > 0)
                             ? (rendered * 100000ull) / elapsed_ms
                             : 0;
    uint64_t us_per_frame    = (rendered > 0)
                             ? (elapsed_ns / 1000ull) / rendered
                             : 0;
    uint64_t bytes_per_frame = (rendered > 0)
                             ? g_bytes_rendered / rendered
                             : 0;
    uint64_t uart_us_per_frame = (bytes_per_frame * 10ull * 1000000ull) / 115200ull;

    printf("shadebobs: %llu frames in %llu.%03llu s%s\n",
           (unsigned long long)rendered,
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           g_do_write ? "" : " (NODRAW)");
    printf("  %llu.%02llu FPS, %llu us/frame\n",
           (unsigned long long)(fps_x100 / 100),
           (unsigned long long)(fps_x100 % 100),
           (unsigned long long)us_per_frame);
    printf("  output: %llu bytes/frame, %llu total bytes "
           "(%llu us/frame at 115200 baud)\n",
           (unsigned long long)bytes_per_frame,
           (unsigned long long)g_bytes_rendered,
           (unsigned long long)uart_us_per_frame);

    free(intensity);
    free(dirty);
    free(dirty_flag);
    return 0;
}

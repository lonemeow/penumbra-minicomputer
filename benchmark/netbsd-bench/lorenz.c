/* lorenz.c - real-time Lorenz attractor in fixed-point arithmetic.
 *
 * Integrates the Lorenz '63 equations forward in time using Euler's
 * method, plotting each new (x, z) point as it's computed.  The
 * trajectory traces the famous butterfly curve gradually, building
 * density over thousands of steps — the "drawing itself out" effect
 * is part of the appeal.
 *
 * Unlike full-screen redraw demos, each step writes only ~30 bytes
 * (cursor positioning + color escape + half-block), so the UART is
 * not the bottleneck and the demo runs at a watchable pace even on
 * a 115200-baud serial console.  The CPU side, on the other hand,
 * does ~7 multiplies per integration step (σ(y-x), x(ρ-z), x·y, β·z,
 * plus three `state += dstate·dt` updates) — so on Penumbra today,
 * with software MUL, this is the most multiplier-bound demo of the
 * set, and the points/second number is a clean signal for measuring
 * the eventual hardware MUL/DIV unit.
 *
 * Math: classic parameters σ=10, ρ=28, β=8/3, Euler dt=0.005.  Q12.20
 * fixed-point throughout (range ±2048, precision ~10⁻⁶).  The Lorenz
 * attractor for these params lives within x ∈ ±22, z ∈ [0, 55] — well
 * inside Q12.20's range with comfortable overflow margin.
 *
 * Usage: lorenz [-m|--mono | -b|--blocks] [-n|--nodraw]
 *               [MAX_STEPS] [WIDTH] [HEIGHT]
 *   defaults: blocks on TTY / mono when piped, infinite steps,
 *             78 x 39 cells
 *
 *   MAX_STEPS = 0 (or omitted) runs until SIGINT or a keypress.
 *   --nodraw computes the trajectory but writes no output (isolates
 *   compute time from I/O time, same convention as plasma).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <termios.h>

/* ── Q12.20 fixed-point helpers ─────────────────────────────────────── */

typedef int32_t q_t;

#define Q_FRAC_BITS  20
#define Q_ONE        ((q_t)1 << Q_FRAC_BITS)
#define Q_INT(n)     ((q_t)((n) * (int64_t)Q_ONE))
#define Q_FROM_FRAC(num, den) \
    ((q_t)(((int64_t)(num) * (int64_t)Q_ONE) / (den)))

static inline q_t qmul(q_t a, q_t b) {
    return (q_t)(((int64_t)a * (int64_t)b) >> Q_FRAC_BITS);
}

/* ── Lorenz '63 parameters and step ─────────────────────────────────── */

#define LORENZ_SIGMA   Q_INT(10)
#define LORENZ_RHO     Q_INT(28)
#define LORENZ_BETA    Q_FROM_FRAC(8, 3)            /* 8/3 ≈ 2.667    */
#define LORENZ_DT      Q_FROM_FRAC(5, 1000)         /* 0.005          */

/* View window for the (x, z) projection.  The attractor stays within
 * these bounds for the classic parameters; anything outside is
 * clipped (and indicates either a numerical excursion or you've
 * fiddled with the params). */
#define VIEW_X_LO      Q_INT(-22)
#define VIEW_X_HI      Q_INT(22)
#define VIEW_Z_LO      Q_INT(0)
#define VIEW_Z_HI      Q_INT(55)

struct lorenz_state {
    q_t x, y, z;
};

static inline void lorenz_step(struct lorenz_state *s) {
    /* Classic Lorenz '63:
     *   dx/dt = σ(y − x)
     *   dy/dt = x(ρ − z) − y
     *   dz/dt = xy − βz
     * Euler integration with fixed dt.  Computes all three
     * derivatives from the *current* state, then commits — the
     * canonical explicit Euler shape. */
    q_t dx = qmul(LORENZ_SIGMA, s->y - s->x);
    q_t dy = qmul(s->x, LORENZ_RHO - s->z) - s->y;
    q_t dz = qmul(s->x, s->y) - qmul(LORENZ_BETA, s->z);
    s->x += qmul(dx, LORENZ_DT);
    s->y += qmul(dy, LORENZ_DT);
    s->z += qmul(dz, LORENZ_DT);
}

/* ── ANSI color palette (rainbow hue cycle) ─────────────────────────── */

static const uint8_t palette[] = {
    196, 202, 208, 214, 220, 226,
    190, 154, 118,  82,  46,
     47,  48,  49,  50,  51,
     45,  39,  33,  27,  21,
     57,  93, 129, 165, 201,
    200, 199, 198, 197,
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))
#define COLOR_BG     16    /* xterm true black — canvas color */

/* ── Precomputed decimal strings for 0..255 ──────────────────────────
 *
 * Used for both color indices (`\033[38;5;NNN`) and cursor positions
 * (`\033[ROW;COLH`).  Eliminates divides from the plot hot path. */
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

/* ── Measurement instrumentation (same convention as plasma) ────────── */

static int g_do_write = 1;
static uint64_t g_bytes_rendered = 0;

static inline void emit_bytes(const char *buf, size_t n) {
    g_bytes_rendered += n;
    if (g_do_write) (void)write(STDOUT_FILENO, buf, n);
}

/* ── Screen state + plotting ────────────────────────────────────────── */

/* screen[col * pixel_rows + row] holds the xterm color index at each
 * half-pixel.  Initialized to COLOR_BG so unplotted halves render as
 * the canvas; updated whenever a point lands there. */
static uint8_t *screen;
static int screen_width;
static int screen_pixel_rows;

static void screen_alloc(int width, int pixel_rows) {
    screen_width      = width;
    screen_pixel_rows = pixel_rows;
    screen = malloc((size_t)width * (size_t)pixel_rows);
    if (!screen) { perror("malloc"); exit(1); }
    memset(screen, COLOR_BG, (size_t)width * (size_t)pixel_rows);
}

/* Plot one half-pixel in blocks mode.  Sets the appropriate half of
 * cell (sx, sy/2), reads back BOTH halves' current colors, and emits
 * a single combined escape + half-block character.  This is the path
 * that gives a multi-color cell when the trajectory paints over an
 * already-colored neighbor. */
static void plot_blocks(int sx, int sy, int color) {
    if (sx < 0 || sx >= screen_width
     || sy < 0 || sy >= screen_pixel_rows) return;

    screen[sx * screen_pixel_rows + sy] = (uint8_t)color;

    int cy  = sy >> 1;
    int top = screen[sx * screen_pixel_rows + (cy << 1)];
    int bot = screen[sx * screen_pixel_rows + (cy << 1) + 1];

    /* Build the escape: cursor-position + fg+bg color + UTF-8 half-block.
     * Worst case: 2+3+1+3+1 + 7+3+6+3 + 1 + 3 = 33 bytes. */
    char buf[48];
    char *out = buf;

    /* \033[ROW;COLH (rows/cols are 1-based in ANSI) */
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, cy + 1);
    *out++ = ';';
    out = emit_dec(out, sx + 1);
    *out++ = 'H';

    /* \033[38;5;F;48;5;Bm */
    *out++ = '\033'; *out++ = '[';
    *out++ = '3'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, top);
    *out++ = ';';
    *out++ = '4'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, bot);
    *out++ = 'm';

    /* U+2580 UPPER HALF BLOCK */
    *out++ = '\xe2'; *out++ = '\x96'; *out++ = '\x80';

    emit_bytes(buf, (size_t)(out - buf));
}

/* Bresenham's line algorithm in blocks mode.  Steps from (x0,y0) to
 * (x1,y1) plotting each pixel; plot_blocks() handles clipping so
 * off-screen segments are harmless.  Used to fill the visible gaps
 * between consecutive integration steps — the trajectory reads as a
 * continuous trace rather than a sparse point cloud. */
static void plot_line_blocks(int x0, int y0, int x1, int y1, int color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        plot_blocks(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Plot one cell in mono mode.  No color, no half-blocks — just '*'
 * at the cell.  Use this when stdout isn't a TTY (or with --mono). */
static void plot_mono(int sx, int sy) {
    if (sx < 0 || sx >= screen_width
     || sy < 0 || sy >= screen_pixel_rows) return;
    /* Mono uses screen_pixel_rows == height (cell rows), so sy is
     * already a cell row.  (set up in main() based on mode) */
    char buf[16];
    char *out = buf;
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, sy + 1);
    *out++ = ';';
    out = emit_dec(out, sx + 1);
    *out++ = 'H';
    *out++ = '*';
    emit_bytes(buf, (size_t)(out - buf));
}

/* Bresenham line in mono mode; same shape as plot_line_blocks. */
static void plot_line_mono(int x0, int y0, int x1, int y1) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        plot_mono(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ── Comet-trail effect (blocks mode) ────────────────────────────────
 *
 * The newest TRAIL_LEN segments are painted in a "comet" gradient on
 * top of the z-colored permanent canvas — white at the head, fading
 * through yellow/orange to red at the tail.  Each integration step:
 *   1. The oldest segment "leaves" the trail and is repainted in its
 *      original z_color, restoring the permanent canvas.
 *   2. The new segment is appended at the head slot.
 *   3. All in-trail segments are redrawn with brightnesses shifted
 *      down by one (head=brightest, tail=dimmest).
 *
 * Costs ~(TRAIL_LEN + 1) line redraws per integration step — modest
 * extra UART bandwidth for the visual win of "you can see where the
 * trajectory IS right now," even after the canvas has saturated. */
#define TRAIL_LEN  8

static const uint8_t trail_palette[TRAIL_LEN] = {
    231,    /* head: white            */
    229,    /*       pale yellow      */
    227,    /*       light yellow     */
    226,    /*       yellow           */
    220,    /*       gold             */
    214,    /*       orange           */
    208,    /*       bright orange    */
    202,    /* tail: orange-red       */
};

struct trail_seg {
    int prev_sx, prev_sy;
    int curr_sx, curr_sy;
    int z_color;
};
static struct trail_seg trail_ring[TRAIL_LEN];
static int trail_head  = 0;   /* slot the next segment will go into */
static int trail_count = 0;   /* current valid entries, 0..TRAIL_LEN */

/* Add a new segment to the comet trail.  Repaints the departing
 * segment (if any) with its z_color first, then repaints every
 * in-trail segment at its now-shifted brightness — so the head is
 * always trail_palette[0] and the tail is trail_palette[count-1]. */
static void trail_advance(int prev_sx, int prev_sy,
                          int sx, int sy, int z_color) {
    int slot = trail_head;
    if (trail_count == TRAIL_LEN) {
        struct trail_seg *old = &trail_ring[slot];
        plot_line_blocks(old->prev_sx, old->prev_sy,
                         old->curr_sx, old->curr_sy, old->z_color);
    } else {
        trail_count++;
    }
    trail_ring[slot].prev_sx = prev_sx;
    trail_ring[slot].prev_sy = prev_sy;
    trail_ring[slot].curr_sx = sx;
    trail_ring[slot].curr_sy = sy;
    trail_ring[slot].z_color = z_color;
    trail_head = (trail_head + 1) % TRAIL_LEN;

    /* Repaint trail head-first, age 0 = newest just-added, age N-1 = oldest. */
    for (int k = 0; k < trail_count; k++) {
        int idx = (trail_head - 1 - k + TRAIL_LEN) % TRAIL_LEN;
        struct trail_seg *seg = &trail_ring[idx];
        plot_line_blocks(seg->prev_sx, seg->prev_sy,
                         seg->curr_sx, seg->curr_sy,
                         trail_palette[k]);
    }
}

/* ── Projection ─────────────────────────────────────────────────────── */

static inline int project_x(q_t x, int width) {
    if (x < VIEW_X_LO || x >= VIEW_X_HI) return -1;
    return (int)(((int64_t)(x - VIEW_X_LO) * (width - 1))
                 / (VIEW_X_HI - VIEW_X_LO));
}

static inline int project_z(q_t z, int pixel_rows) {
    if (z < VIEW_Z_LO || z >= VIEW_Z_HI) return -1;
    /* Flip so high z is at the *top* of the screen (matches how
     * Lorenz figures are conventionally drawn). */
    int proj = (int)(((int64_t)(z - VIEW_Z_LO) * (pixel_rows - 1))
                     / (VIEW_Z_HI - VIEW_Z_LO));
    return (pixel_rows - 1) - proj;
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
    int max_steps = 0;        /* 0 = infinite until SIGINT/keypress */
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
        /* Reserved for future preset arg; for now reject unknown. */
        fprintf(stderr, "%s: unexpected arg '%s'\n", argv[0], argv[argi]);
        return 2;
    }
    if (argi < argc) max_steps = atoi(argv[argi++]);
    if (argi < argc) width     = atoi(argv[argi++]);
    if (argi < argc) height    = atoi(argv[argi++]);

    if (width < 8 || height < 4 || max_steps < 0) {
        fprintf(stderr,
                "usage: %s [-b|--blocks | -m|--mono] [-n|--nodraw] "
                       "[MAX_STEPS] [WIDTH>=8] [HEIGHT>=4]\n",
                argv[0]);
        return 2;
    }

    init_dec();

    int pixel_rows = (mode == MODE_BLOCKS) ? 2 * height : height;
    screen_alloc(width, pixel_rows);

    /* Trap SIGINT for clean exit. */
    struct sigaction sa = { .sa_handler = sigint_handler };
    sigaction(SIGINT, &sa, NULL);

    /* Non-canonical stdin for "press any key to stop." */
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

    /* Banner. */
    printf("Penumbra Lorenz: σ=10, ρ=28, β=8/3, dt=0.005, Q12.20\n");
    printf("  %dx%d cells (%dx%d samples), mode=%s, max_steps=%s%s\n",
           width, height, width, pixel_rows,
           mode == MODE_BLOCKS ? "half-block + 256-color" : "mono ASCII",
           max_steps == 0 ? "infinite" : "limited",
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

    /* Initial conditions (1, 1, 1) — far from the attractor, so the
     * first few hundred steps trace the spiral-in transient before
     * settling onto the butterfly shape.  That itself is fun to
     * watch as a demo. */
    struct lorenz_state s = { Q_INT(1), Q_INT(1), Q_INT(1) };

    uint64_t t_start = now_ns();
    uint64_t step = 0;
    /* Previous-point state for line drawing.  prev_valid stays 0 on
     * the very first iteration (no line to draw yet) and stays 0
     * across off-screen excursions where we can't anchor a segment. */
    int prev_sx = 0, prev_sy = 0, prev_valid = 0;
    while (!stop_flag && (max_steps == 0 || (int)step < max_steps)) {
        lorenz_step(&s);

        int sx = project_x(s.x, width);
        int sy = project_z(s.z, pixel_rows);

        /* Color by altitude (z value) instead of step number — paints
         * coherent z-bands across the picture, so revisiting the same
         * region paints it the same color rather than overwriting
         * with cycled rainbow noise.  One integer mul + one DIV per
         * step (the DIV is software today; cheap enough for one-per
         * -step granularity). */
        int color_idx = (int)(((int64_t)(s.z - VIEW_Z_LO)
                              * (int64_t)PALETTE_LEN)
                              / (VIEW_Z_HI - VIEW_Z_LO));
        if (color_idx < 0) color_idx = 0;
        if (color_idx >= (int)PALETTE_LEN) color_idx = (int)PALETTE_LEN - 1;
        int color = palette[color_idx];

        if (mode == MODE_BLOCKS) {
            /* For the very first step, prev_* aren't set — treat it
             * as a zero-length segment (Bresenham emits one pixel).
             * The trail will track this as a one-pixel "segment"
             * which rolls off normally. */
            if (!prev_valid) { prev_sx = sx; prev_sy = sy; }
            trail_advance(prev_sx, prev_sy, sx, sy, color);
        } else {
            if (prev_valid) plot_line_mono(prev_sx, prev_sy, sx, sy);
            else            plot_mono(sx, sy);
        }
        prev_sx = sx;
        prev_sy = sy;
        prev_valid = 1;

        step++;

        /* Drain stdout periodically so points appear as they're
         * plotted rather than buffered up.  Every 16 steps is a
         * compromise: too frequent and stdio overhead bites; too
         * rare and the animation feels chunky. */
        if (g_do_write && (step & 15) == 0) fflush(stdout);

        if (kbd_active) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) stop_flag = 1;
        }
    }
    uint64_t t_end = now_ns();

    /* Restore terminal. */
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
    uint64_t pts_x100_per_s  = (elapsed_ms > 0)
                             ? (step * 100000ull) / elapsed_ms
                             : 0;
    uint64_t us_per_step     = (step > 0)
                             ? (elapsed_ns / 1000ull) / step
                             : 0;
    uint64_t bytes_per_step  = (step > 0)
                             ? g_bytes_rendered / step
                             : 0;
    uint64_t uart_us_per_step = (bytes_per_step * 10ull * 1000000ull) / 115200ull;

    printf("lorenz: %llu steps in %llu.%03llu s%s\n",
           (unsigned long long)step,
           (unsigned long long)(elapsed_ns / 1000000000ull),
           (unsigned long long)((elapsed_ns % 1000000000ull) / 1000000ull),
           g_do_write ? "" : " (NODRAW)");
    printf("  %llu.%02llu points/s, %llu us/step\n",
           (unsigned long long)(pts_x100_per_s / 100),
           (unsigned long long)(pts_x100_per_s % 100),
           (unsigned long long)us_per_step);
    printf("  output: %llu bytes/step, %llu total bytes "
           "(%llu us/step at 115200 baud)\n",
           (unsigned long long)bytes_per_step,
           (unsigned long long)g_bytes_rendered,
           (unsigned long long)uart_us_per_step);

    free(screen);
    return 0;
}

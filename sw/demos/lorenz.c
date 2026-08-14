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

#include "demo.h"
#include "dterm.h"

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




/* ── Measurement instrumentation (same convention as plasma) ────────── */

static inline void emit_bytes(const char *buf, size_t n) {
    (void)write(STDOUT_FILENO, buf, n);
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

    out = dterm_at(out, cy + 1, sx + 1);

    out = dterm_pair(out, top, bot);

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
    out = dterm_at(out, sy + 1, sx + 1);
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

/* ── Framework glue ─────────────────────────────────────────────────── */

/*
 * Integration steps per second of wall clock.  The attractor is drawn
 * by its own trajectory, so this sets how fast the curve is traced
 * rather than how much happens per frame — the shape appears at the
 * same rate whether a frame costs a screenful of escapes or a few
 * hundred stores.
 */
#define STEPS_PER_SEC  600

struct lorenz_demo {
    struct lorenz_state s;
    uint64_t elapsed_us;
    uint64_t stepped;       /* integration steps taken so far */
    int prev_sx, prev_sy;
    int prev_valid;
    int ready;
};

static struct lorenz_demo state = {
    { Q_INT(1), Q_INT(1), Q_INT(1) },   /* far from the attractor, so the
                                         * spiral-in transient is visible */
    0, 0, 0, 0, 0, 0
};

static int setup(const struct demo_surface *s) {
    screen_width = (int)s->width;
    screen_pixel_rows = (s->kind == DEMO_PIXEL) ? (int)s->height
                                                : (int)s->height * 2;
    screen_alloc(screen_width, screen_pixel_rows);
    state.ready = 1;
    return 0;
}

/* Colour by altitude rather than step number: revisiting a region
 * paints it the same colour, so the picture builds coherent z-bands
 * instead of cycling noise over itself. */
static int color_for(q_t z) {
    int idx = (int)(((int64_t)(z - VIEW_Z_LO) * (int64_t)PALETTE_LEN)
        / (VIEW_Z_HI - VIEW_Z_LO));

    if (idx < 0)
        idx = 0;
    if (idx >= (int)PALETTE_LEN)
        idx = (int)PALETTE_LEN - 1;
    return idx;
}

/* Advance the trajectory to where elapsed time says it should be.
 * `plot` receives each new screen point and its palette index. */
static void step_to_now(struct lorenz_demo *d,
                        void (*plot)(int, int, int, int, int))
{
    uint64_t want = (d->elapsed_us * STEPS_PER_SEC) / 1000000ull;

    while (d->stepped < want) {
        int sx, sy, idx;

        lorenz_step(&d->s);
        d->stepped++;

        sx = project_x(d->s.x, screen_width);
        sy = project_z(d->s.z, screen_pixel_rows);
        idx = color_for(d->s.z);

        if (!d->prev_valid) {
            d->prev_sx = sx;
            d->prev_sy = sy;
        }
        plot(d->prev_sx, d->prev_sy, sx, sy, idx);
        d->prev_sx = sx;
        d->prev_sy = sy;
        d->prev_valid = 1;
    }
}

/* ── Cell surface ───────────────────────────────────────────────────── */

static int cell_blocks;   /* the surface's half-block capability */

static int cell_first = 1;

static void plot_cells(int x0, int y0, int x1, int y1, int idx) {
    if (cell_blocks) {
        trail_advance(x0, y0, x1, y1, palette[idx]);
    } else if (cell_first) {
        /* Nothing to join the first point to. */
        plot_mono(x1, y1);
        cell_first = 0;
    } else {
        /* The trajectory can cross several cells between steps, so
         * the segment is drawn rather than its endpoint plotted. */
        plot_line_mono(x0, y0, x1, y1);
    }
}

static void frame_cells(const struct demo_surface *s, uint32_t dt_us,
                        uint64_t frame, void *vs) {
    struct lorenz_demo *d = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
        cell_blocks = s->blocks;
    }
    d->elapsed_us += dt_us;
    step_to_now(d, plot_cells);
}

/* ── Pixel surface ──────────────────────────────────────────────────── */

static uint8_t *fb_pix;
static unsigned fb_stride;
static unsigned fb_w, fb_h;

/* Bresenham between successive trajectory points: at this step rate the
 * curve moves more than a pixel per step in the fast parts of the
 * orbit, and plotting only the endpoints would leave it dashed. */
static void plot_pixels(int x0, int y0, int x1, int y1, int idx) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && (unsigned)x0 < fb_w &&
            y0 >= 0 && (unsigned)y0 < fb_h)
            fb_pix[(size_t)y0 * fb_stride + x0] = (uint8_t)(idx + 1);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* The palette's colours, one per entry, with 0 kept as the canvas.
 * A terminal leaves untouched cells alone; every pixel here has a
 * colour, so the background has to be one. */
static void build_cmap(const struct demo_surface *s) {
    uint8_t r[256], g[256], b[256];
    unsigned n = s->cmap_entries < 256 ? s->cmap_entries : 256;

    memset(r, 0, sizeof(r));
    memset(g, 0, sizeof(g));
    memset(b, 0, sizeof(b));
    for (unsigned i = 0; i < PALETTE_LEN && i + 1 < n; i++)
        demo_xterm_rgb(palette[i], &r[i + 1], &g[i + 1], &b[i + 1]);
    demo_set_cmap(s, r, g, b);
}

static void frame_pixels(const struct demo_surface *s, uint32_t dt_us,
                         uint64_t frame, void *vs) {
    struct lorenz_demo *d = vs;

    if (frame == 0) {
        if (setup(s) != 0)
            exit(1);
        build_cmap(s);
        fb_pix = s->pix;
        fb_stride = s->stride;
        fb_w = s->width;
        fb_h = s->height;
    }
    d->elapsed_us += dt_us;
    step_to_now(d, plot_pixels);
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name        = "lorenz",
        .frame_cell  = frame_cells,
        .frame_pixel = frame_pixels,
        .state       = &state,
    };

    return demo_main(argc, argv, &d);
}

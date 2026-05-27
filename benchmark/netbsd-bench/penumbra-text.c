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

static uint8_t *intensity;
static int g_width;
static int g_pixel_rows;
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
    mark_dirty(col, row);
}

static void emit_cell_blocks(int col, int cell_row) {
    int top_i = intensity[col * g_pixel_rows + (cell_row << 1)];
    int bot_i = intensity[col * g_pixel_rows + (cell_row << 1) + 1];
    int fg = intensity_to_color(top_i);
    int bg = intensity_to_color(bot_i);

    char buf[48];
    char *out = buf;

    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, cell_row + 1);
    *out++ = ';';
    out = emit_dec(out, col + 1);
    *out++ = 'H';

    *out++ = '\033'; *out++ = '[';
    *out++ = '3'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, fg);
    *out++ = ';';
    *out++ = '4'; *out++ = '8'; *out++ = ';';
    *out++ = '5'; *out++ = ';';
    out = emit_dec(out, bg);
    *out++ = 'm';

    *out++ = '\xe2'; *out++ = '\x96'; *out++ = '\x80';

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
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, plasma_cell_row + 1);
    *out++ = ';';
    out = emit_dec(out, plasma_x0 + 1);
    *out++ = 'H';

    if (mode_is_blocks) {
        int last_color = -1;
        for (int x = 0; x < plasma_width; x++) {
            int v = sin_lut[(uint8_t)(x * 3 + t)]
                  + sin_lut[(uint8_t)(x * 5 + t * 3)];
            int color = plasma_color_lut[v + 254];
            if (color != last_color) {
                *out++ = '\033'; *out++ = '[';
                *out++ = '4'; *out++ = '8'; *out++ = ';';
                *out++ = '5'; *out++ = ';';
                out = emit_dec(out, color);
                *out++ = 'm';
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
    *out++ = '\033'; *out++ = '[';
    out = emit_dec(out, cell_row + 1);
    *out++ = ';';
    out = emit_dec(out, col + 1);
    *out++ = 'H';
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

static void build_text_pixels(int screen_w, int screen_h) {
    text_width_px = NUM_LETTERS * LETTER_W + (NUM_LETTERS - 1) * LETTER_GAP;
    text_x0    = (screen_w - text_width_px) / 2;
    int text_y0 = (screen_h - LETTER_H) / 2;
    if (text_x0  < 0) text_x0  = 0;
    if (text_y0  < 0) text_y0  = 0;

    /* Plasma underline geometry.  Sits a few pixel rows below the
     * text bottom, slightly wider, on a single cell row.  Only used
     * if there's room on screen — otherwise we silently skip it. */
    int plasma_pixel_y = text_y0 + LETTER_H + PLASMA_GAP_PIXELS;
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

    int cap = NUM_LETTERS * LETTER_W * LETTER_H;
    text_pixels = malloc(sizeof(*text_pixels) * (size_t)cap);
    if (!text_pixels) { perror("malloc"); exit(1); }
    text_pixel_count = 0;
    for (int letter = 0; letter < NUM_LETTERS; letter++) {
        int lx = text_x0 + letter * (LETTER_W + LETTER_GAP);
        for (int row = 0; row < LETTER_H; row++) {
            uint8_t bits = glyphs[letter][row];
            for (int col = 0; col < LETTER_W; col++) {
                if (bits & (uint8_t)(1u << (LETTER_W - 1 - col))) {
                    text_pixels[text_pixel_count].x = (int16_t)(lx + col);
                    text_pixels[text_pixel_count].y = (int16_t)(text_y0 + row);
                    text_pixel_count++;
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
static const int splat_weights[3][3] = {
    { 4,  8,  4 },
    { 8, 64,  8 },
    { 4,  8,  4 },
};

static void splat(int x, int y) {
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            splat_pixel(x + dx, y + dy, splat_weights[dy + 1][dx + 1]);
        }
    }
}

/* ── Animation state machine ────────────────────────────────────────── */

enum phase {
    PHASE_TRACE,    /* bob walks the text pixels, splatting at each */
    PHASE_HOLD,     /* text saturated, holding the bright picture   */
};
static int phase;
static int phase_frame;
static int path_idx;    /* index into text_pixels[]              */
static int pass_num;    /* completed full-traversal passes       */

#define BOB_SPEED      2     /* text pixels visited per frame */
#define MAX_PASSES     4     /* enough passes to saturate (4*64 ≥ 255) */
#define HOLD_FRAMES  120     /* ~4 seconds at 30 FPS UART */

/* Reset the screen and start a fresh trace cycle.  Uses \033[2J full
 * clear so we don't have to dirty every text cell — single 11-byte
 * escape vs hundreds of per-cell escapes.
 *
 * Critical: \033[0m goes *before* \033[2J.  Erase escapes (2J, J, K)
 * fill with the current background color, and after a trace frame
 * our SGR state has whatever bg the last cell set — often a bright
 * one for cells whose text pixel was in the bottom half.  Without
 * the reset, the screen flips to that bright bg color and stays
 * there for the next cycle. */
static void reset_screen(void) {
    memset(intensity, 0, (size_t)g_width * (size_t)g_pixel_rows);
    static const char clear_esc[] = "\033[0m\033[2J\033[H";
    emit_bytes(clear_esc, sizeof(clear_esc) - 1);
    phase = PHASE_TRACE;
    phase_frame = 0;
    path_idx = 0;
    pass_num = 0;
}

static void step_animation(void) {
    if (phase == PHASE_HOLD) {
        phase_frame++;
        if (phase_frame >= HOLD_FRAMES) {
            reset_screen();
        }
        return;
    }

    /* PHASE_TRACE: bob visits BOB_SPEED text pixels this frame,
     * splatting at each.  When the path completes we start a fresh
     * pass; after MAX_PASSES passes the centers have saturated and
     * we transition to HOLD. */
    for (int i = 0; i < BOB_SPEED; i++) {
        if (path_idx >= text_pixel_count) {
            pass_num++;
            if (pass_num >= MAX_PASSES) {
                phase = PHASE_HOLD;
                phase_frame = 0;
                return;
            }
            path_idx = 0;
        }
        splat(text_pixels[path_idx].x, text_pixels[path_idx].y);
        path_idx++;
    }
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
/* Reset SGR before clear so erase doesn't fill with a stale bright bg
 * (see reset_screen() for the full explanation). */
static void clear_screen(void)  { fputs("\033[0m\033[2J\033[H", stdout); }
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

    init_dec();
    init_sin_lut();
    init_plasma_lut();

    g_width      = width;
    g_cell_rows  = height;
    g_pixel_rows = (mode == MODE_BLOCKS) ? 2 * height : height;

    intensity = calloc((size_t)g_width * (size_t)g_pixel_rows, 1);
    dirty_capacity = NUM_LETTERS * LETTER_H + 16;
    dirty = malloc(sizeof(*dirty) * (size_t)dirty_capacity);
    dirty_flag = calloc((size_t)g_width * (size_t)g_cell_rows, 1);
    if (!intensity || !dirty || !dirty_flag) { perror("malloc"); return 1; }

    build_text_pixels(width, g_pixel_rows);

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

    printf("Penumbra Text Screensaver: %dx%d cells (%dx%d samples), %s%s\n",
           width, height, width, g_pixel_rows,
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

    phase = PHASE_TRACE;
    phase_frame = 0;
    path_idx = 0;
    pass_num = 0;

    uint64_t t_start = now_ns();
    uint64_t rendered = 0;
    while (!stop_flag && (frames == 0 || (int)rendered < frames)) {
        /* Reset dirty tracking for this frame. */
        for (int i = 0; i < dirty_count; i++) {
            int cell_idx = dirty[i].col * g_cell_rows + dirty[i].row;
            dirty_flag[cell_idx] = 0;
        }
        dirty_count = 0;

        step_animation();

        /* Re-emit just the dirty cells. */
        for (int i = 0; i < dirty_count; i++) {
            int col = dirty[i].col;
            int row = dirty[i].row;
            if (mode == MODE_BLOCKS) emit_cell_blocks(col, row);
            else                     emit_cell_mono(col, row);
        }

        /* Plasma underline animates every frame, including during
         * HOLD — keeps motion in the picture so the screensaver
         * never feels frozen. */
        emit_plasma_line((int)rendered, mode == MODE_BLOCKS);

        if (g_do_write) fflush(stdout);
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

    printf("penumbra-text: %llu frames in %llu.%03llu s%s\n",
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
    free(text_pixels);
    return 0;
}

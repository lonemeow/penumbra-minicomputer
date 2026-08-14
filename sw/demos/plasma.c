/* plasma.c - demoscene plasma effect + UART throughput measurement.
 *
 * Two precomputed fields of smooth wavy values, slid over one another
 * and added — the classic "computer art from 1992" look, done the way
 * the era did it.  The frame loop is two loads, an add and a store per
 * pixel: no sines, no multiplies, no divides, and no clamping, because
 * the palette is a ring and the 8-bit sum is allowed to wrap round it.
 *
 * It remains a full-screen redraw, so on a 115200-baud serial console
 * the wire, not the CPU, sets the frame rate — tens of KB per frame at
 * 78x39 in half-block colour.  On a framebuffer there is no wire and
 * the effect finally runs at the speed it was written for.
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

#include "demo.h"
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

/* ── The engine ─────────────────────────────────────────────────────── */
/*
 * Two precomputed fields of smooth wavy values, slid over one another
 * and added.  The whole frame loop is then two loads, an add and a
 * store per pixel — no sines, no multiplies, no divides on the hot
 * path, which is what makes a full-screen effect affordable here.
 *
 * The add wraps at 8 bits and nothing clamps or normalises it.  That
 * works because the palette is a *cycle*: entry 255 flows back into
 * entry 0, so a wrap is not an artifact to be avoided but simply the
 * colour continuing round.  Clamping would need a compare per pixel
 * and normalising a divide; wrapping needs neither.
 *
 * The fields are oversized by a margin so the offsets have somewhere
 * to roam.  Each frame picks a position for each field, and only the
 * row pointers are computed per row — the inner loop never multiplies.
 */

#define TICKS_PER_SEC 25          /* how fast the fields slide */

/*
 * Three things arrange themselves so the frame loop is pointer walking
 * and nothing else:
 *
 *  - The field row pitch is a power of two, so stepping a row is a
 *    shift the compiler folds into the address rather than a multiply.
 *  - Horizontal offsets are multiples of four, which keeps every one
 *    of the three pointers word-aligned however the fields are slid —
 *    an unaligned 32-bit load is a fault here, not a slow path.
 *  - Field values are prescaled to 0..127, so four packed sums can be
 *    added as one 32-bit word with no carry crossing a byte lane.
 *    That is what buys four pixels per add.
 */
/*
 * Field A exists four times over, each built one pixel further along
 * than the last.  Reading copy (ax & 3) at aligned offset (ax & ~3)
 * gives exactly offset ax — so the slide is smooth to the pixel while
 * every load stays word-aligned.  Without it the horizontal offset has
 * to be a multiple of four and the picture visibly steps.
 *
 * Field B slides vertically only, where any row is already reachable,
 * so it needs no such copies.
 */
static uint8_t *field_a[4], *field_b;
static int field_w;        /* row pitch, a power of two */
static int field_shift;    /* log2 of it, so a row step is a shift */
static int field_h;
static int field_margin;   /* how far the fields can be slid */

/* Scratch for the cell renderer's two half-pixel rows.  Word-aligned
 * so blend_row's packed adds apply there too. */
static uint8_t *sum_top, *sum_bot;

/*
 * One radial ripple field, filled without a single multiply or square
 * root in the loop.
 *
 * Walking x, the squared distance changes by a known amount — d2 goes
 * up by 2*dx+1 — and its square root changes by at most one step,
 * because the root of a slowly growing number grows slowly.  So the
 * root is *carried* rather than computed: keep r and r*r alongside,
 * and nudge them when d2 has moved far enough to warrant it.  The
 * lookup index is carried the same way, incremented by the frequency
 * whenever r moves, which is what removes the last multiply.
 *
 * Written the obvious way instead — a square root per pixel, itself a
 * loop with a multiply in it — this took roughly forty seconds to
 * build one field at 320x240, which reads as the demo not working.
 */
static void fill_radial(uint8_t *buf, int cx, int cy, int freq) {
    for (int y = 0; y < field_h; y++) {
        uint8_t *row = buf + (size_t)y * field_w;
        int dy = y - cy;
        int dx = -cx;
        int d2 = dx * dx + dy * dy;
        int r = 0, rsq = 0, ridx = 0;

        for (int x = 0; x < field_w; x++) {
            /* Carry r = floor(sqrt(d2)).  (r+1)^2 is rsq + 2r + 1, and
             * r^2 is rsq - (2(r-1) + 1), so neither direction needs a
             * multiply. */
            while (rsq + 2 * r + 1 <= d2) {
                rsq += 2 * r + 1;
                r++;
                ridx += freq;
            }
            while (r > 0 && rsq > d2) {
                r--;
                rsq -= 2 * r + 1;
                ridx -= freq;
            }
            /* Halved, so two fields sum to at most 254 and a word-wide
             * add never carries out of a byte. */
            row[x] = (uint8_t)((sin_lut[ridx & 0xFF] + 128) >> 1);

            d2 += 2 * dx + 1;
            dx++;
        }
    }
}

/*
 * Two radial ripple patterns with different centres and wavelengths.
 * Concentric rings interfering with concentric rings is what gives
 * plasma its curved cell structure; two grids of straight waves would
 * read as a plaid.
 */
static int build_fields(int screen_w, int screen_pixel_rows) {
    /* Room to slide proportional to the picture, so the image travels
     * a useful fraction of the screen rather than a fixed few dozen
     * pixels that barely register at 320 across. */
    field_margin = (screen_w / 2) & ~3;
    if (field_margin < 32)
        field_margin = 32;

    field_w = 1;
    field_shift = 0;
    while (field_w < screen_w + field_margin) {
        field_w <<= 1;
        field_shift++;
    }
    field_h = screen_pixel_rows + field_margin;

    for (int i = 0; i < 4; i++) {
        field_a[i] = malloc((size_t)field_w * field_h);
        if (field_a[i] == NULL) {
            perror("malloc");
            return -1;
        }
    }
    field_b = malloc((size_t)field_w * field_h);
    if (field_b == NULL) {
        perror("malloc");
        return -1;
    }

    /* Rounded up to a word so the blend's tail never runs. */
    sum_top = malloc((size_t)((screen_w + 3) & ~3));
    sum_bot = malloc((size_t)((screen_w + 3) & ~3));
    if (sum_top == NULL || sum_bot == NULL) {
        perror("malloc");
        return -1;
    }

    /* Shifting the centre one pixel left shifts the whole pattern one
     * pixel right, which is what makes copy i the field seen from
     * offset i. */
    for (int i = 0; i < 4; i++)
        fill_radial(field_a[i], field_w / 3 - i, field_h / 2, 5);
    fill_radial(field_b, (field_w * 3) / 4, field_h / 5, 3);
    return 0;
}

/* Where each field sits this tick.  Lissajous paths, so the two drift
 * against each other without ever settling into a repeat that reads as
 * one. */
static void field_offsets(int t, int *ax, int *ay, int *bx, int *by) {
    int half = field_margin / 2;

    /* A travels both axes, to the pixel — the pre-shifted copies make
     * an odd horizontal offset as cheap as an even one.  B travels
     * vertically, where alignment never mattered. */
    *ax = half + ((half * sin_lut[(t * 2) & 0xFF]) >> 7);
    *ay = half + ((half * sin_lut[(t * 3 + 64) & 0xFF]) >> 7);
    *bx = half;
    *by = half + ((half * sin_lut[(t * 4 + 96) & 0xFF]) >> 7);
}

/*
 * Add one row of two fields into a destination, four pixels per
 * iteration.  Every pointer is word-aligned by construction, and the
 * prescaled fields mean the packed add cannot carry between lanes.
 */
static inline void blend_row(uint8_t *dst, const uint8_t *a,
                             const uint8_t *b, unsigned n) {
    uint32_t *d32 = (uint32_t *)dst;
    const uint32_t *a32 = (const uint32_t *)a;
    const uint32_t *b32 = (const uint32_t *)b;
    unsigned words = n >> 2;

    while (words--)
        *d32++ = *a32++ + *b32++;

    /* Whatever the width leaves over. */
    for (unsigned x = n & ~3u; x < n; x++)
        dst[x] = (uint8_t)(a[x] + b[x]);
}

/* ── State ──────────────────────────────────────────────────────────── */

struct plasma {
    const struct palette *pal;
    uint64_t elapsed_us;
};

static struct plasma state = { NULL, 0 };

static int parse_args(int argc, char **argv, void *vs) {
    struct plasma *p = vs;

    if (argc > 0 && strcmp(argv[0], "list") == 0) {
        list_palettes();
        exit(0);
    }
    if (argc > 0) {
        p->pal = find_palette(argv[0]);
        if (p->pal == NULL) {
            fprintf(stderr, "unknown palette '%s' (try `list')\n", argv[0]);
            return -1;
        }
    } else {
        p->pal = &palettes[0];
    }
    return 0;
}

/* ── Cell surface ───────────────────────────────────────────────────── */

static void frame_cells(const struct demo_surface *s, uint32_t dt_us,
                        uint64_t frame, void *vs) {
    struct plasma *p = vs;
    int ax, ay, bx, by, t;
    char *line;

    if (frame == 0) {
        init_sin_lut();
        if (build_fields((int)s->width, (int)s->height * 2) != 0)
            exit(1);
    }
    p->elapsed_us += dt_us;
    t = (int)((p->elapsed_us * TICKS_PER_SEC) / 1000000ull);
    field_offsets(t, &ax, &ay, &bx, &by);

    line = malloc((size_t)s->width * 32 + 16);
    if (line == NULL) { perror("malloc"); exit(1); }

    dterm_home();
    const uint8_t *a0 = field_a[ax & 3] + ((size_t)ay << field_shift)
        + (ax & ~3);
    const uint8_t *b0 = field_b + ((size_t)by << field_shift) + bx;

    for (unsigned cy = 0; cy < s->height; cy++) {
        char *out = line;
        int last_fg = -2, last_bg = -2;

        /* Both half-pixel rows blended first, so the emit loop below
         * only picks colours. */
        blend_row(sum_top, a0, b0, s->width);
        blend_row(sum_bot, a0 + field_w, b0 + field_w, s->width);

        for (unsigned x = 0; x < s->width; x++) {
            uint8_t vt = sum_top[x];
            uint8_t vb = sum_bot[x];
            int fg = p->pal->table[(vt * p->pal->len) >> 8];
            int bg = p->pal->table[(vb * p->pal->len) >> 8];

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
        fputs(line, stdout);
        a0 += field_w * 2;
        b0 += field_w * 2;
    }
    free(line);
}

/* ── Pixel surface ──────────────────────────────────────────────────── */

/*
 * The colormap is the palette resampled to every entry and closed into
 * a ring: entry 255 blends back into entry 0, so the sum wrapping is
 * invisible.  That property is what lets the inner loop add without
 * checking anything.
 */
static void build_cmap(const struct demo_surface *s, const struct palette *pal)
{
    uint8_t r[256], g[256], b[256];
    unsigned n = s->cmap_entries < 256 ? s->cmap_entries : 256;

    for (unsigned i = 0; i < n; i++) {
        unsigned pos = i * (unsigned)pal->len * 256 / n;
        unsigned stop = (pos >> 8) % pal->len;
        unsigned next = (stop + 1) % pal->len;    /* wraps: a ring */
        unsigned frac = pos & 0xFF;
        uint8_t r0, g0, b0, r1, g1, b1;

        demo_xterm_rgb(pal->table[stop], &r0, &g0, &b0);
        demo_xterm_rgb(pal->table[next], &r1, &g1, &b1);
        r[i] = (uint8_t)((r0 * (256 - frac) + r1 * frac) >> 8);
        g[i] = (uint8_t)((g0 * (256 - frac) + g1 * frac) >> 8);
        b[i] = (uint8_t)((b0 * (256 - frac) + b1 * frac) >> 8);
    }
    demo_set_cmap(s, r, g, b);
}

static void frame_pixels(const struct demo_surface *s, uint32_t dt_us,
                         uint64_t frame, void *vs) {
    struct plasma *p = vs;
    int ax, ay, bx, by, t;

    if (frame == 0) {
        init_sin_lut();
        if (build_fields((int)s->width, (int)s->height) != 0)
            exit(1);
        build_cmap(s, p->pal);
    }
    p->elapsed_us += dt_us;
    t = (int)((p->elapsed_us * TICKS_PER_SEC) / 1000000ull);
    field_offsets(t, &ax, &ay, &bx, &by);

    const uint8_t *a = field_a[ax & 3] + ((size_t)ay << field_shift)
        + (ax & ~3);
    const uint8_t *b = field_b + ((size_t)by << field_shift) + bx;
    uint8_t *d = s->pix;

    for (unsigned y = 0; y < s->height; y++) {
        blend_row(d, a, b, s->width);
        a += field_w;
        b += field_w;
        d += s->stride;
    }
}

int main(int argc, char **argv) {
    static const struct demo d = {
        .name        = "plasma",
        .frame_cell  = frame_cells,
        .frame_pixel = frame_pixels,
        .parse       = parse_args,
        .usage_tail  = "[PALETTE]\n       plasma list",
        .state       = &state,
    };

    return demo_main(argc, argv, &d);
}

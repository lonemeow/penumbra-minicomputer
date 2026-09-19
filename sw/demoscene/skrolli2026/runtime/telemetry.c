/*
 * telemetry.c — a stacked bar of where the part's cycles went.
 *
 * The stall buckets are exclusive and a retiring cycle is charged to
 * none, so the segments partition the cycles.  CPI is printed over the
 * bar's left end from 55 bytes of digits cut from the display font,
 * rather than the 4 KB font a general text overlay would pull through
 * the L1-D the running part is using.
 *
 * Counts are summed over a window of frames and the bar recomputed
 * once per window, so a fast part does not flicker it; the sum of
 * partitions is still a partition, so the bar is exact for the window
 * rather than smoothed.  It is redrawn every frame all the same, since
 * the part may have drawn over it.
 */

#include "rt.h"

#define BAR_SEGMENTS   (RT_FB_W / 4)      /* one word per segment */
#define BAR_STRIDE     (RT_FB_W / 4)      /* words per pixel row */
#define BAR_ROWS_FB    8                  /* doubled: one cell row tall */
#define WINDOW_HZ      10

/* CPI as x.xx, one cell in from the left in either mode: a cell is 8
 * screen pixels, and so is a framebuffer word. */
#define TEXT_COL       1
#define TEXT_MAX       8                  /* "32767.99" */
#define TEXT_INK       (RT_PAL_TELEMETRY + RT_PERF_BUCKETS)
#define GLYPH_ROWS     5
#define GLYPH_DOT      10
#define TEXT_ROW_FB    ((BAR_ROWS_FB - GLYPH_ROWS) / 2)

/* tools/telefont.py: digits 0-9 then '.', 8 wide, bit 7 leftmost. */
extern const unsigned char rt_telefont[11][GLYPH_ROWS];

/* In rt_perf_delta() order.  Each rgb is the built-in CGA value of its
 * text_fg, so the bar matches across a mode cut.  hazard and flush share
 * a colour on purpose: forwarding makes hazard stalls rare. */
static const struct {
    const char *label;
    uint32_t    rgb;
    uint8_t     text_fg;
} bar_seg[RT_PERF_BUCKETS] = {
    { "retired", 0x00AAAAAAu, RT_COL_GREY },
    { "load",    0x000000AAu, RT_COL_BLUE },
    { "store",   0x0000AAAAu, RT_COL_CYAN },
    { "ifetch",  0x00AA0000u, RT_COL_RED },
    { "funit",   0x00FFFF55u, RT_COL_YELLOW },
    { "hazard",  0x00FF5555u, RT_COL_LTRED },
    { "flush",   0x00FF5555u, RT_COL_LTRED },
};

void rt_palette_reserved(void)
{
    for (int i = 0; i < RT_PERF_BUCKETS; i++)
        rt_fb_palette(RT_PAL_TELEMETRY + i, bar_seg[i].rgb);
    rt_fb_palette(TEXT_INK, 0x00000000u);
}

_Static_assert(BAR_SEGMENTS == RT_TEXT_W, "one cell per segment in text mode");

/* Rows unrolled at constant displacements: one STW per row, and the
 * colour word built once per segment rather than once per row. */
_Static_assert(BAR_ROWS_FB == 8, "the unrolled store list assumes eight rows");
_Static_assert(TEXT_INK < 256, "the text ink needs a reserved palette slot");
_Static_assert(TEXT_COL + 2 * TEXT_MAX <= BAR_SEGMENTS, "text fits the bar");

/* A glyph row's nibble as a word mask: its bit 3 is the leftmost pixel,
 * which is the word's low byte. */
#define NIB(n) ((((n) >> 3 & 1) * 0x000000FFu) | (((n) >> 2 & 1) * 0x0000FF00u) | \
                (((n) >> 1 & 1) * 0x00FF0000u) | (((n)      & 1) * 0xFF000000u))
static const uint32_t nibble_mask[16] = {
    NIB(0),  NIB(1),  NIB(2),  NIB(3),  NIB(4),  NIB(5),  NIB(6),  NIB(7),
    NIB(8),  NIB(9),  NIB(10), NIB(11), NIB(12), NIB(13), NIB(14), NIB(15),
};

/* Segments hold bucket indices; each mode maps them to its colours. */
static uint8_t  seg[BAR_SEGMENTS];
static uint32_t acc[RT_PERF_BUCKETS], acc_total;
static uint32_t window_start, window_ticks;
static int      recompute_now;

/* Glyph indices: a digit's own value, or GLYPH_DOT. */
static uint8_t  text[TEXT_MAX];
static int      text_len;

static void draw_fb(void)
{
    volatile uint32_t *p = rt_fb_words()
                         + (RT_FB_H - BAR_ROWS_FB) * BAR_STRIDE;

    for (int i = 0; i < BAR_SEGMENTS; i++) {
        uint32_t w = (uint32_t)(RT_PAL_TELEMETRY + seg[i]) * 0x01010101u;

        p[0 * BAR_STRIDE] = w;
        p[1 * BAR_STRIDE] = w;
        p[2 * BAR_STRIDE] = w;
        p[3 * BAR_STRIDE] = w;
        p[4 * BAR_STRIDE] = w;
        p[5 * BAR_STRIDE] = w;
        p[6 * BAR_STRIDE] = w;
        p[7 * BAR_STRIDE] = w;
        p++;
    }
}

/* Each glyph covers two segments; ink replaces the bar under its pixels. */
static void draw_text_fb(void)
{
    volatile uint32_t *p = rt_fb_words()
                         + (RT_FB_H - BAR_ROWS_FB + TEXT_ROW_FB) * BAR_STRIDE;
    uint32_t ink = (uint32_t)TEXT_INK * 0x01010101u;

    for (int i = 0; i < text_len; i++) {
        const unsigned char *g = rt_telefont[text[i]];
        int w = TEXT_COL + 2 * i;
        uint32_t left  = (uint32_t)(RT_PAL_TELEMETRY + seg[w])     * 0x01010101u;
        uint32_t right = (uint32_t)(RT_PAL_TELEMETRY + seg[w + 1]) * 0x01010101u;

        for (int r = 0; r < GLYPH_ROWS; r++) {
            uint32_t ml = nibble_mask[g[r] >> 4];
            uint32_t mr = nibble_mask[g[r] & 0xF];

            p[r * BAR_STRIDE + w]     = (left & ~ml)  | (ink & ml);
            p[r * BAR_STRIDE + w + 1] = (right & ~mr) | (ink & mr);
        }
    }
}

/* Black on the bar's own colour: the attribute's background is free. */
static void draw_cells(void)
{
    volatile uint32_t *row = rt_cells() + (RT_TEXT_H - 1) * RT_TEXT_W;

    for (int i = 0; i < BAR_SEGMENTS; i++)
        row[i] = RT_CELL(0xDB, bar_seg[seg[i]].text_fg, 0);
    for (int i = 0; i < text_len; i++) {
        int c = TEXT_COL + i;
        int glyph = text[i] == GLYPH_DOT ? '.' : '0' + text[i];

        row[c] = RT_CELL(glyph, RT_COL_BLACK, bar_seg[seg[c]].text_fg);
    }
}

static void set_cpi(uint32_t cycles, uint32_t retired)
{
    uint8_t digits[5];
    int n = 0;

    text_len = 0;
    if (retired == 0)
        return;

    /* Both are plain counts, so their 16.16 quotient is the ratio. */
    fix16_t cpi = fix16_div((fix16_t)cycles, (fix16_t)retired);
    uint32_t whole = (uint32_t)fix16_to_int(cpi);
    uint32_t hundredths = ((uint32_t)fix16_frac(cpi) * 100 + 0x8000) >> 16;

    if (hundredths == 100) {
        whole++;
        hundredths = 0;
    }
    do {
        digits[n++] = (uint8_t)(whole % 10);
        whole /= 10;
    } while (whole);

    while (n)
        text[text_len++] = digits[--n];
    text[text_len++] = GLYPH_DOT;
    text[text_len++] = (uint8_t)(hundredths / 10);
    text[text_len++] = (uint8_t)(hundredths % 10);
}

static void recompute(void)
{
    uint32_t total = acc_total, run = 0, filled = 0;
    int shift = 0;

    if (total == 0)
        return;

    /* run * BAR_SEGMENTS must fit: only a window of well over a second
     * of cycles needs scaling down. */
    while ((total >> shift) > 0xFFFFFFFFu / BAR_SEGMENTS)
        shift++;

    for (int i = 0; i < RT_PERF_BUCKETS; i++) {
        uint32_t edge;

        run += acc[i];
        edge = ((run >> shift) * BAR_SEGMENTS) / (total >> shift);
        if (edge > BAR_SEGMENTS)
            edge = BAR_SEGMENTS;
        while (filled < edge)
            seg[filled++] = (uint8_t)i;
    }

    /* Rounding down leaves the tail short; it belongs to the last bucket. */
    while (filled < BAR_SEGMENTS)
        seg[filled++] = RT_PERF_BUCKETS - 1;
}

void rt_telemetry_restart(void)
{
    for (int i = 0; i < RT_PERF_BUCKETS; i++)
        acc[i] = 0;
    acc_total = 0;
    window_ticks = rt_tick_hz() / WINDOW_HZ;
    recompute_now = 1;
    text_len = 0;
}

void rt_telemetry_draw(const rt_perf_t *before, const rt_perf_t *after,
                       rt_mode_t mode)
{
    uint32_t d[RT_PERF_BUCKETS];
    uint32_t now = rt_ticks();

    acc_total += rt_perf_delta(before, after, d);
    for (int i = 0; i < RT_PERF_BUCKETS; i++)
        acc[i] += d[i];

    if (recompute_now || now - window_start >= window_ticks) {
        recompute();
        set_cpi(acc_total, acc[0]);
        for (int i = 0; i < RT_PERF_BUCKETS; i++)
            acc[i] = 0;
        acc_total = 0;
        window_start = now;
        recompute_now = 0;
    }

    if (mode == RT_MODE_FB) {
        draw_fb();
        draw_text_fb();
    } else {
        draw_cells();
    }
}

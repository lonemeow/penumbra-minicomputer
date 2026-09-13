/*
 * telemetry.c — a stacked bar of where the last frame's cycles went.
 *
 * The stall buckets are exclusive and a retiring cycle is charged to
 * none, so the segments partition the cycles.  Colour only: a font
 * read would evict half the L1-D the running part is using.
 */

#include "rt.h"

#define BAR_SEGMENTS   (RT_FB_W / 4)      /* one word per segment */
#define BAR_STRIDE     (RT_FB_W / 4)      /* words per pixel row */
#define BAR_ROWS_FB    8                  /* doubled: one cell row tall */

/* In rt_perf_delta() order.  Each rgb is the built-in CGA value of its
 * text_fg, so the bar matches across a mode cut.  hazard and flush share
 * a colour on purpose: forwarding makes hazard stalls rare. */
static const struct {
    const char *label;
    uint32_t    rgb;
    uint8_t     text_fg;
} bar_seg[RT_PERF_BUCKETS] = {
    { "retired", 0x00AAAAAAu, 7 },
    { "load",    0x000000AAu, 1 },
    { "store",   0x0000AAAAu, 3 },
    { "ifetch",  0x00AA0000u, 4 },
    { "funit",   0x00FFFF55u, 14 },
    { "hazard",  0x00FF5555u, 12 },
    { "flush",   0x00FF5555u, 12 },
};

void rt_palette_reserved(void)
{
    for (int i = 0; i < RT_PERF_BUCKETS; i++)
        rt_fb_palette(RT_PAL_TELEMETRY + i, bar_seg[i].rgb);
}

_Static_assert(BAR_SEGMENTS == RT_TEXT_W, "one cell per segment in text mode");
_Static_assert(BAR_SEGMENTS < 256, "BAR_SEGMENTS << 24 must fit 32 bits");

/* Rows unrolled at constant displacements: one STW per row, and the
 * colour word built once per segment rather than once per row. */
_Static_assert(BAR_ROWS_FB == 8, "the unrolled store list assumes eight rows");

static void draw_fb(const uint8_t *seg)
{
    volatile uint32_t *p = rt_fb_words()
                         + (RT_FB_H - BAR_ROWS_FB) * BAR_STRIDE;

    for (int i = 0; i < BAR_SEGMENTS; i++) {
        uint32_t w = (uint32_t)seg[i] * 0x01010101u;

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

static void draw_cells(const uint8_t *seg)
{
    volatile uint32_t *row = rt_cells() + (RT_TEXT_H - 1) * RT_TEXT_W;

    for (int i = 0; i < BAR_SEGMENTS; i++)
        row[i] = RT_CELL(0xDB, seg[i], 0);
}

void rt_telemetry_draw(const rt_perf_t *before, const rt_perf_t *after,
                       rt_mode_t mode)
{
    uint32_t d[RT_PERF_BUCKETS];
    uint8_t  seg[BAR_SEGMENTS];
    uint32_t total = rt_perf_delta(before, after, d);

    if (total == 0)
        return;

    /* Q24: a Q16 reciprocal of a frame's cycle count keeps only two or
     * three bits.  run <= total keeps run * recip under BAR_SEGMENTS << 24. */
    uint32_t recip = ((uint32_t)BAR_SEGMENTS << 24) / total;
    uint32_t run = 0, filled = 0;
    int fb = (mode == RT_MODE_FB);

    for (int i = 0; i < RT_PERF_BUCKETS; i++) {
        uint8_t c = fb ? (uint8_t)(RT_PAL_TELEMETRY + i) : bar_seg[i].text_fg;
        uint32_t edge;

        run += d[i];
        edge = (run * recip) >> 24;
        if (edge > BAR_SEGMENTS)
            edge = BAR_SEGMENTS;
        while (filled < edge)
            seg[filled++] = c;
    }

    /* Truncation leaves the tail short; it belongs to the last bucket. */
    uint8_t tail = fb ? (uint8_t)(RT_PAL_TELEMETRY + RT_PERF_BUCKETS - 1)
                      : bar_seg[RT_PERF_BUCKETS - 1].text_fg;
    while (filled < BAR_SEGMENTS)
        seg[filled++] = tail;

    if (fb)
        draw_fb(seg);
    else
        draw_cells(seg);
}

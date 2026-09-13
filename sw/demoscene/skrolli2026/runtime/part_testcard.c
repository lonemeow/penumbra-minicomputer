/*
 * part_testcard.c — bring-up part, not demo content.  One packed
 * full-screen redraw per frame: the floor for any full-screen effect.
 */

#include "rt.h"

extern const rt_part_t part_testcard;

static void enter(void)
{
    /* 128 entries, so the frame wraps with a mask. */
    for (uint32_t i = 0; i < 128; i++)
        rt_fb_palette(i, (i << 17) | ((127 - i) << 9) | ((i * 2) ^ 0xFF));
}

static void frame(uint32_t t)
{
    volatile uint32_t *fb = rt_fb_words();
    uint32_t phase = t >> 4;

    for (uint32_t y = 0; y < RT_FB_H; y++) {
        volatile uint32_t *row = fb + y * (RT_FB_W / 4);
        uint32_t base = (y + phase) & 0xFF;

        for (uint32_t x = 0; x < RT_FB_W / 4; x++) {
            uint32_t p = (base + x) & 0x7F;
            row[x] = p | (p << 8) | (p << 16) | (p << 24);
        }
    }
}

const rt_part_t part_testcard = {
    .name        = "testcard",
    .mode        = RT_MODE_FB,
    .duration_frames = 90,   /* three seconds at 30 fps */
    .frame       = frame,
    .enter       = enter,
};

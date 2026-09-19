/* image.c — images compiled by tools/png2image.py. */

#include "rt.h"

void rt_image_palette(const rt_image_t *img)
{
    if (img->colors > RT_PAL_RESERVED)
        rt_panic("image palette reaches the telemetry's slots");

    for (uint32_t i = 0; i < img->colors; i++)
        rt_fb_palette(i, img->palette[i]);
}

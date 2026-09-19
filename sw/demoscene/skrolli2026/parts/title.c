#include "rt.h"
#include "wclogo_image.h"

static void setup(void)
{
    memset((void *)rt_fb_words(), 0, 320 * 240);
    rt_image_palette(&wclogo_image);
}

static void cleanup(void)
{
}

static void render_frame(fix16_t now, fix16_t start, fix16_t end)
{
    int offset = (320 - wclogo_image.w) / 2;
    uint8_t *fb_p = (uint8_t *)rt_fb_words();
    const uint8_t *img_p = wclogo_image.pixels;
    for (int y = 0; y < wclogo_image.h; y++) {
        int row_offset = offset + fix16_to_int(fix16_mul(rt_sin(y), F16(20)));
        const uint8_t *row_img_p = img_p;
        uint8_t *row_fb_p = fb_p;
        int row_w = wclogo_image.w;
        if (row_offset < 0) {
            row_img_p += -row_offset;
            row_w -= -row_offset;
        } else {
            row_fb_p += row_offset;
            if (row_offset + row_w > 320)
                row_w = 320 - row_offset;
        }
        if (row_w > 0)
            memcpy(row_fb_p, row_img_p, row_w);
        fb_p += 320;
        img_p += wclogo_image.stride;
    }
}

const rt_part_t part_title = {
    .name  = "title",
    .mode  = RT_MODE_FB,
    .frame = render_frame,
    .enter = setup,
    .leave = cleanup
};

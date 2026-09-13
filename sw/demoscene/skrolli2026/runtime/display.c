/* display.c — CLASS_DISPLAY bring-up and access. */

#include "rt_internal.h"

/* ── Device registers (doc/system/devices/display.md) ─────────── */
#define PD_CAP          0x000
#define PD_INFO         0x004
#define PD_CTRL         0x008
#define PD_FB_GEOM      0x01C
#define PD_FB_FORMAT    0x020
#define PD_CELLS        0x1000
#define PD_FB_PALETTE   0x10000
#define PD_FB           0x20000

#define PD_CAP_FRAMEBUFFER  (1u << 13)
#define PD_CTRL_ENABLE      (1u << 0)
#define PD_CTRL_FB_SEL      (1u << 4)

static uint32_t   disp_base;
static uint32_t   ctrl_shadow;
static rt_mode_t  mode_now = RT_MODE_TEXT;

#define REG(off)  (*(volatile uint32_t *)(disp_base + (off)))

void rt_display_init(uint32_t bootdata)
{
    if (!rt_find_device(bootdata, ACFG_CLASS_DISPLAY, &disp_base, 0))
        rt_panic("no CLASS_DISPLAY device");

    if (!(REG(PD_CAP) & PD_CAP_FRAMEBUFFER))
        rt_panic("display has no framebuffer");

    if ((REG(PD_FB_FORMAT) & 0xFF) != 8)
        rt_panic("framebuffer is not 8bpp indexed");

    uint32_t geom = REG(PD_FB_GEOM);
    if ((geom & 0xFFFF) != RT_FB_W || ((geom >> 16) & 0xFFFF) != RT_FB_H)
        rt_panic("framebuffer geometry is not 320x240");

    uint32_t info = REG(PD_INFO);
    if ((info & 0xFFFF) != RT_TEXT_W || ((info >> 16) & 0xFFFF) != RT_TEXT_H)
        rt_panic("character console is not 80x30");

    /* Off until drawn: display memory is undefined at power-up. */
    ctrl_shadow = 0;
    REG(PD_CTRL) = ctrl_shadow;
}

void rt_display_mode(rt_mode_t m)
{
    mode_now = m;
    if (m == RT_MODE_FB)
        ctrl_shadow |= PD_CTRL_FB_SEL;
    else
        ctrl_shadow &= ~PD_CTRL_FB_SEL;
    REG(PD_CTRL) = ctrl_shadow;
}

rt_mode_t rt_display_current_mode(void)
{
    return mode_now;
}

void rt_display_enable(int on)
{
    if (on)
        ctrl_shadow |= PD_CTRL_ENABLE;
    else
        ctrl_shadow &= ~PD_CTRL_ENABLE;
    REG(PD_CTRL) = ctrl_shadow;
}

volatile uint32_t *rt_fb_words(void)
{
    return (volatile uint32_t *)(disp_base + PD_FB);
}

volatile uint32_t *rt_cells(void)
{
    return (volatile uint32_t *)(disp_base + PD_CELLS);
}

void rt_fb_palette(uint32_t index, uint32_t rgb)
{
    *(volatile uint32_t *)(disp_base + PD_FB_PALETTE + index * 4) = rgb;
}

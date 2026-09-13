/* display.c — CLASS_DISPLAY bring-up and access. */

#include "rt.h"
#include "penumbra.h"

/* ── Bootdata (mirrors hw/rom/bootdata.h) ─────────────────────── */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2
#define ACFG_CLASS_DISPLAY  6

struct btag_hdr {
    uint32_t type;
    uint32_t size;
};

struct btag_device {
    struct btag_hdr hdr;
    uint32_t cls;
    uint32_t base;
    uint32_t dev_size;
    uint32_t id;
    char     name[16];
};

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

static uint32_t find_device(uint32_t bootdata, uint32_t cls)
{
    /* Skip the header: magic, version, total_size. */
    uint32_t p = (bootdata ? bootdata : BOOTDATA_BASE) + 12;

    for (;;) {
        const struct btag_hdr *h = (const struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        /* A tag too small to advance the cursor would loop forever. */
        if (h->size < sizeof(struct btag_hdr))
            return 0;
        if (h->type == BTAG_DEVICE) {
            const struct btag_device *d = (const struct btag_device *)p;
            if (d->cls == cls)
                return d->base;
        }
        p += h->size;
    }
}

void rt_display_init(uint32_t bootdata)
{
    disp_base = find_device(bootdata, ACFG_CLASS_DISPLAY);
    if (!disp_base)
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

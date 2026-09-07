/*
 * display.h — CLASS_DISPLAY register map.
 *
 * The offsets and capability bits from
 * doc/system/devices/display.md, shared by the conformance checks and
 * the bandwidth characterization so the two cannot drift apart.
 *
 * Only the subset both files use is here; the class defines more
 * (modes, soft font, cursor) that nothing in this directory touches.
 */

#ifndef FBTEST_DISPLAY_H
#define FBTEST_DISPLAY_H

/* Register block */
#define PD_CAP          0x000
#define PD_INFO         0x004
#define PD_CTRL         0x008
#define PD_FB_GEOM      0x01C
#define PD_FB_FORMAT    0x020

/* Apertures */
#define PD_FB_PALETTE   0x10000
#define PD_FB           0x20000

/* CAP bits */
#define PD_CAP_FRAMEBUFFER  (1u << 13)

/* CTRL bits */
#define PD_CTRL_ENABLE      (1u << 0)
#define PD_CTRL_FB_SEL      (1u << 4)

#endif /* FBTEST_DISPLAY_H */

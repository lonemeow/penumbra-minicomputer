/*
 * bandwidth.h — access-cost characterization for the display aperture.
 */

#ifndef FBTEST_BANDWIDTH_H
#define FBTEST_BANDWIDTH_H

#include "bench.h"

/*
 * Measure and report the cost of reaching the pixel aperture: each
 * access width, both cache mappings, and SDRAM alongside for
 * comparison.  Leaves the framebuffer full of measurement garbage, so
 * call it before whatever draws the final picture.
 */
void fb_bandwidth_report(uint32_t base, uint32_t width, uint32_t height);

#endif /* FBTEST_BANDWIDTH_H */

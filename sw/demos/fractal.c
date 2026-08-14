/* fractal.c — shared colour handling for the escape-time demos.
 *
 * See fractal.h.  The cell path picks from xterm's fixed 256; the pixel
 * path owns the whole colormap, which is why the two read differently
 * here — one selects, the other defines.
 */

#include <stdint.h>

#include "fractal.h"

const char fractal_ramp[] = " .:-=+*#%@";

/* ── ANSI 256-color palette ──────────────────────────────────────────
 *
 * 30-step hue cycle through xterm's 256-color cube (indices 16..231
 * form a 6x6x6 RGB grid where idx = 16 + 36*r + 6*g + b for r,g,b
 * in [0..5]).  These stops trace
 *   red -> yellow -> green -> cyan -> blue -> magenta -> red.
 *
 * Pairing this with the brightness-ramp character gives a two-channel
 * cell: color hue = iteration band, character density = iteration
 * count.  Both signals stack visibly at typical font sizes.
 *
 * Swap-out options for different aesthetics:
 *   fire:    16, 52, 88, 124, 160, 196, 202, 208, 214, 220, 226
 *   ocean:   16, 17, 18, 19, 20, 21, 27, 33, 39, 45, 51
 *   gray:    232..255 (xterm's 24-step gray ramp) */
static const uint8_t palette[] = {
    196, 202, 208, 214, 220, 226,      /* red    -> yellow  */
    190, 154, 118,  82,  46,           /* yellow -> green   */
     47,  48,  49,  50,  51,           /* green  -> cyan    */
     45,  39,  33,  27,  21,           /* cyan   -> blue    */
     57,  93, 129, 165, 201,           /* blue   -> magenta */
    200, 199, 198, 197,                /* magenta-> red     */
};
#define PALETTE_LEN  (sizeof(palette) / sizeof(palette[0]))

/* Color used for "inside the set" cells in blocks mode (both fg and
 * bg get this on in-set halves).  16 is the true-black corner of the
 * 6x6x6 cube — renders as solid silhouette on light backgrounds and
 * "voids in the rainbow" on dark backgrounds.  Both look good. */
#define COLOR_INSET   16

/* Map iter count to an xterm 256-color index.
 *
 * `iter % PALETTE_LEN` cycles the palette — produces tight color
 * bands that trace the equipotentials around the boundary, which is
 * the visually striking choice for zoomed views.  For a smooth
 * gradient instead, use `palette[(iter * PALETTE_LEN) / max_iter]`. */
uint8_t fractal_cell_color(int iter, int max_iter) {
    if (iter >= max_iter) return COLOR_INSET;
    return palette[iter % PALETTE_LEN];
}


/* ── Pixel colormap ───────────────────────────────────────────────── */

/* Iteration count to colormap entry.  Bounded rather than
 * allocated — see FRACTAL_MAX_ITER. */
static uint8_t iter_to_cmap_idx[FRACTAL_MAX_ITER + 1];

void fractal_build_cmap(uint8_t *r, uint8_t *g, uint8_t *b,
                        unsigned cmsize, int max_iter)
{
    for (unsigned i = 0; i < cmsize; i++) {
        r[i] = 0x00;
        g[i] = 0x00;
        b[i] = 0x00;
    }

    for (int i = 0; i < max_iter; i++) {
        int cm_idx = (int)(((unsigned)i * (cmsize - 1)) / (unsigned)max_iter);
        iter_to_cmap_idx[i] = cm_idx;

        double t = (double)cm_idx / 255.0;
        double rv = 9.0  * (1.0 - t) * t * t * t;
        double gv = 15.0 * (1.0 - t) * (1.0 - t) * t * t;
        double bv = 8.5  * (1.0 - t) * (1.0 - t) * (1.0 - t) * t;

        r[cm_idx] = (uint8_t)(rv * 255.0);
        g[cm_idx] = (uint8_t)(gv * 255.0);
        b[cm_idx] = (uint8_t)(bv * 255.0);
    }

    // Non-escaping
    iter_to_cmap_idx[max_iter] = cmsize - 1;
    r[cmsize - 1] = 0x00;
    g[cmsize - 1] = 0x00;
    b[cmsize - 1] = 0x00;
}

uint8_t fractal_pixel_index(int iter)
{
    return iter_to_cmap_idx[iter];
}


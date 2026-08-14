/* fractal.h — shared pieces of the escape-time fractal demos.
 *
 * Mandelbrot and Julia differ in one line of arithmetic: which of z and
 * c comes from the pixel.  Everything around that — the fixed-point
 * format, the escape test, and how an iteration count becomes a colour
 * on either surface — is the same, and lives here rather than in each.
 *
 * Q4.28 fixed-point:
 *   bit 31      sign
 *   bits 30..28 integer part (effective range +-8)
 *   bits 27..0  28 fractional bits (~3.7e-9 resolution)
 *
 * Penumbra has no FP hardware, so the whole kernel is integer: a 64-bit
 * intermediate for the multiply, shifted back down.  That is also what
 * makes these demos a clean stress test of the 32x32 -> 64 multiply path.
 */

#ifndef FRACTAL_H
#define FRACTAL_H

#include <stdint.h>

/* ── Q4.28 fixed point ────────────────────────────────────────────── */

typedef int32_t q_t;

#define Q_FRAC_BITS  28
#define Q_ONE        ((q_t)1 << Q_FRAC_BITS)

/* Convert a small integer literal at compile time. */
#define Q_INT(n)     ((q_t)((n) * (int64_t)Q_ONE))

/* Escape threshold: |z|^2 > 4. */
#define Q_ESCAPE     Q_INT(4)

/* Q4.28 from a rational num/den, for viewport coordinates that are not
 * integer multiples of Q_ONE.  Multiplies rather than shifts, since
 * shifting a negative signed value is implementation-defined. */
#define Q_FROM_FRAC(num, den) \
    ((q_t)(((int64_t)(num) * (int64_t)Q_ONE) / (den)))

/* Multiply two Q4.28 values.  The 64-bit intermediate is essential:
 * squaring a value near 2.0 produces a 60+ bit product before the
 * shift. */
static inline q_t qmul(q_t a, q_t b) {
    return (q_t)(((int64_t)a * (int64_t)b) >> Q_FRAC_BITS);
}

/* ── Cell-surface colour ──────────────────────────────────────────── */

/* Brightness ramp for the monochrome renderer: lighter escaped early,
 * '@' never escaped. */
extern const char fractal_ramp[];
#define FRACTAL_RAMP_LEN  10

/* An iteration count as an xterm-256 index, cycling a 30-step hue so
 * the bands trace the equipotentials around the boundary. */
uint8_t fractal_cell_color(int iter, int max_iter);

/* ── Pixel-surface colour ─────────────────────────────────────────── */

/*
 * The largest max_iter these demos accept.  The colour lookup is a
 * table indexed by iteration count, and bounding it keeps that table a
 * fixed array — no allocation to check, none to release, and a value
 * this far above the presets is already beyond what the hardware
 * renders in a sitting.
 */
#define FRACTAL_MAX_ITER  4096

/*
 * Build the colormap for a given iteration range and prepare the
 * lookup that fractal_pixel_index() reads.  Knowing max_iter here is
 * what leaves the per-pixel step a table read rather than a divide:
 * the normalisation happens once, over cmsize entries, instead of
 * 76800 times a frame.
 *
 * r/g/b are cmsize bytes each, ready for the surface's colormap.
 */
void fractal_build_cmap(uint8_t *r, uint8_t *g, uint8_t *b,
                        unsigned cmsize, int max_iter);

/*
 * The colormap entry for an iteration count.  Valid for 0..max_iter
 * inclusive: the iterators return max_iter itself for a point that
 * never escaped, and that value has an entry of its own.
 */
uint8_t fractal_pixel_index(int iter);

#endif /* FRACTAL_H */

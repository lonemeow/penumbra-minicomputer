/*
 * Penumbra display adapter (CLASS_DISPLAY) — boot ROM driver.
 * Contract: doc/system/devices/display.md.
 */

#include "display.h"

/* Register block and the cell aperture, word-strided throughout. */
#define DISP_CAP     0x000
#define DISP_INFO    0x004
#define DISP_CTRL    0x008
#define DISP_CURSOR  0x00C
#define DISP_CELLS   0x1000

#define CTRL_ENABLE     0x1
#define CTRL_CURSOR_EN  0x2

#define CAP_COLOR    (1u << 8)

static inline uint32_t disp_rd(const struct display *d, uint32_t reg) {
    return *(volatile uint32_t *)(d->base + reg);
}

static inline void disp_wr(const struct display *d, uint32_t reg,
                           uint32_t val) {
    *(volatile uint32_t *)(d->base + reg) = val;
}

/* The cell at (row, col): one word-strided slot holding {attr, glyph}. */
static inline void cell_wr(const struct display *d, int row, int col,
                           unsigned char attr, unsigned char glyph) {
    uint32_t off = DISP_CELLS + (uint32_t)(row * d->cols + col) * 4;
    disp_wr(d, off, ((uint32_t)attr << 8) | glyph);
}

void display_attach(struct display *d, uint32_t base) {
    d->base = base;

    uint32_t cap = disp_rd(d, DISP_CAP);
    uint32_t info = disp_rd(d, DISP_INFO);

    d->version = (unsigned char)(cap & 0xFF);
    d->color   = (cap & CAP_COLOR) != 0;
    d->cols    = (int)(info & 0xFFFF);
    d->rows    = (int)((info >> 16) & 0xFFFF);
}

void display_clear(const struct display *d, unsigned char attr) {
    for (int row = 0; row < d->rows; row++)
        for (int col = 0; col < d->cols; col++)
            cell_wr(d, row, col, attr, ' ');
}

void display_puts_at(const struct display *d, int row, int col,
                     unsigned char attr, const char *s) {
    if (row < 0 || row >= d->rows)
        return;
    for (; *s && col < d->cols; s++, col++)
        if (col >= 0)
            cell_wr(d, row, col, attr, (unsigned char)*s);
}

void display_cursor(const struct display *d, int on, int row, int col) {
    uint32_t ctrl = disp_rd(d, DISP_CTRL);

    if (on) {
        disp_wr(d, DISP_CURSOR, ((uint32_t)row << 16) | (uint32_t)col);
        ctrl |= CTRL_CURSOR_EN;
    } else {
        ctrl &= ~CTRL_CURSOR_EN;
    }
    disp_wr(d, DISP_CTRL, ctrl);
}

void display_enable(const struct display *d, int on) {
    uint32_t ctrl = disp_rd(d, DISP_CTRL);

    if (on)
        ctrl |= CTRL_ENABLE;
    else
        ctrl &= ~CTRL_ENABLE;
    disp_wr(d, DISP_CTRL, ctrl);
}

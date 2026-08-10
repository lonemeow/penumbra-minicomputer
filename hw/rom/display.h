/*
 * Penumbra display adapter (CLASS_DISPLAY) — boot ROM driver.
 *
 * The class minimum protocol, enough to put a screen up before any
 * OS exists: geometry discovery, cell writes, and the enable that
 * makes the picture visible (doc/system/devices/display.md).
 *
 * The device powers up blanked with undefined cell contents, so the
 * order is always clear, draw, then enable — otherwise the monitor
 * shows whatever the cell memory happened to hold.
 *
 * No writable data section in ROM: the caller owns the struct.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "penumbra.h"

struct display {
    uint32_t base;      /* MMIO base assigned by autoconfig */
    int      cols;
    int      rows;
    unsigned char version;   /* CAP version field */
    int      color;     /* CAP.COLOR — renders 16 distinct colors */
};

/* Read identity and geometry from the device at base. */
void display_attach(struct display *d, uint32_t base);

/* Fill the whole grid with blanks in the given attribute. */
void display_clear(const struct display *d, unsigned char attr);

/* Write s at (row, col); stops at the row's last column. */
void display_puts_at(const struct display *d, int row, int col,
                     unsigned char attr, const char *s);

/* Show the cursor at (row, col), or hide it when on is 0. */
void display_cursor(const struct display *d, int on, int row, int col);

/* Picture on/off — everything else stays as it was. */
void display_enable(const struct display *d, int on);

#endif /* DISPLAY_H */

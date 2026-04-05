/*
 * bootdata.h — Penumbra boot data tagged list
 *
 * Defines the boot data structure passed from ROM → stage 1 → stage 2
 * via R1 (physical pointer). The structure is a tagged list of
 * variable-length entries starting at BOOTDATA_BASE (0x0040, right
 * after the trap vector table on page 0).
 *
 * All entries are 4-byte aligned. Consumers walk the list by
 * advancing by entry->size bytes. Unknown tags are skipped.
 *
 * No globals — the builder functions take a cursor pointer that
 * tracks the next free byte in the boot data area.
 */

#ifndef BOOTDATA_H
#define BOOTDATA_H

#include "penumbra.h"

/* ── Boot data lives on page 0 after the 16-entry vector table ───── */
#define BOOTDATA_BASE  0x00000040

/* ── Header (first entry in the list) ────────────────────────────── */
#define BOOTDATA_MAGIC   0x50454E42  /* "PENB" */
#define BOOTDATA_VERSION 1

struct bootdata_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;   /* filled in at finalization */
};

/* ── Common entry header ─────────────────────────────────────────── */
struct btag_hdr {
    uint32_t type;
    uint32_t size;         /* total entry size including this header */
};

/* ── Tag types ───────────────────────────────────────────────────── */
#define BTAG_END       0
#define BTAG_DEVICE    2   /* discovered/injected device */
#define BTAG_CONSOLE   3   /* console output device */
#define BTAG_BOOTDEV   4   /* boot storage device */

/* ── BTAG_DEVICE ─────────────────────────────────────────────────── */
struct btag_device {
    struct btag_hdr hdr;   /* type=BTAG_DEVICE, size=40 */
    uint32_t cls;          /* ACFG_CLASS_* */
    uint32_t base;         /* MMIO base address */
    uint32_t dev_size;     /* device size in bytes */
    uint32_t id;           /* device ID from autoconfig */
    char     name[16];     /* null-terminated, padded */
};

/* ── BTAG_CONSOLE ────────────────────────────────────────────────── */
struct btag_console {
    struct btag_hdr hdr;   /* type=BTAG_CONSOLE, size=12 */
    uint32_t dev_nth;      /* index into BTAG_DEVICE entries (0-based) */
};

/* ── BTAG_BOOTDEV ────────────────────────────────────────────────── */
struct btag_bootdev {
    struct btag_hdr hdr;   /* type=BTAG_BOOTDEV, size=20 */
    uint32_t dev_nth;      /* index into BTAG_DEVICE entries */
    uint32_t cs;           /* chip-select pin (for SPI-based devices) */
    uint32_t partition;    /* partition number (0 = raw/whole device) */
};

/* ── BTAG_END ────────────────────────────────────────────────────── */
/* Just a btag_hdr with type=0, size=8. No payload. */

/* ── Builder API ─────────────────────────────────────────────────── *
 *
 * Usage:
 *   uint32_t cursor = bd_init();
 *   bd_add_device(&cursor, ACFG_CLASS_MEMORY, base, size, 0, "RAM");
 *   bd_add_device(&cursor, cls, base, size, id, name);
 *   ...
 *   bd_finalize(&cursor);
 *   // R1 = BOOTDATA_BASE, jump to loader
 */

/* Initialize boot data header, return cursor past it. */
static inline uint32_t bd_init(void) {
    struct bootdata_hdr *hdr = (struct bootdata_hdr *)BOOTDATA_BASE;
    hdr->magic = BOOTDATA_MAGIC;
    hdr->version = BOOTDATA_VERSION;
    hdr->total_size = 0;  /* filled by bd_finalize */
    return BOOTDATA_BASE + sizeof(struct bootdata_hdr);
}

/* Add a device entry. Returns the 0-based device index (position
 * among BTAG_DEVICE entries so far). */
static inline int bd_add_device(uint32_t *cursor,
                                uint32_t cls, uint32_t base,
                                uint32_t dev_size, uint32_t id,
                                const char *name) {
    struct btag_device *e = (struct btag_device *)(*cursor);
    e->hdr.type = BTAG_DEVICE;
    e->hdr.size = sizeof(struct btag_device);
    e->cls = cls;
    e->base = base;
    e->dev_size = dev_size;
    e->id = id;
    /* Copy name (up to 15 chars + NUL) */
    int i;
    for (i = 0; i < 15 && name[i]; i++)
        e->name[i] = name[i];
    for (; i < 16; i++)
        e->name[i] = '\0';
    *cursor += e->hdr.size;

    /* Count how many BTAG_DEVICE entries precede this one */
    int nth = 0;
    uint32_t p = BOOTDATA_BASE + sizeof(struct bootdata_hdr);
    while (p < *cursor - e->hdr.size) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_DEVICE)
            nth++;
        p += h->size;
    }
    return nth;
}

static inline void bd_add_console(uint32_t *cursor, uint32_t dev_nth) {
    struct btag_console *e = (struct btag_console *)(*cursor);
    e->hdr.type = BTAG_CONSOLE;
    e->hdr.size = sizeof(struct btag_console);
    e->dev_nth = dev_nth;
    *cursor += e->hdr.size;
}

static inline void bd_add_bootdev(uint32_t *cursor,
                                  uint32_t dev_nth, uint32_t cs,
                                  uint32_t partition) {
    struct btag_bootdev *e = (struct btag_bootdev *)(*cursor);
    e->hdr.type = BTAG_BOOTDEV;
    e->hdr.size = sizeof(struct btag_bootdev);
    e->dev_nth = dev_nth;
    e->cs = cs;
    e->partition = partition;
    *cursor += e->hdr.size;
}

/* Write BTAG_END and fill in header total_size. */
static inline void bd_finalize(uint32_t *cursor) {
    struct btag_hdr *end = (struct btag_hdr *)(*cursor);
    end->type = BTAG_END;
    end->size = sizeof(struct btag_hdr);
    *cursor += end->size;

    struct bootdata_hdr *hdr = (struct bootdata_hdr *)BOOTDATA_BASE;
    hdr->total_size = *cursor - BOOTDATA_BASE;
}

/* ── Lookup helpers ──────────────────────────────────────────────── */

/* Find the nth BTAG_DEVICE entry (0-based). Returns NULL if not found. */
static inline struct btag_device *bd_find_device(int nth) {
    uint32_t p = BOOTDATA_BASE + sizeof(struct bootdata_hdr);
    int count = 0;
    for (;;) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        if (h->type == BTAG_DEVICE) {
            if (count == nth)
                return (struct btag_device *)p;
            count++;
        }
        p += h->size;
    }
}

/* Given a pointer to a BTAG_DEVICE entry, return its overall 0-based
 * device index (position among all BTAG_DEVICE entries in the list). */
static inline int bd_device_index(struct btag_device *target) {
    uint32_t p = BOOTDATA_BASE + sizeof(struct bootdata_hdr);
    int count = 0;
    for (;;) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return -1;
        if (h->type == BTAG_DEVICE) {
            if ((struct btag_device *)p == target)
                return count;
            count++;
        }
        p += h->size;
    }
}

/* Find the nth BTAG_DEVICE with a given class. Returns NULL if not found. */
static inline struct btag_device *bd_find_device_by_class(uint32_t cls, int nth) {
    uint32_t p = BOOTDATA_BASE + sizeof(struct bootdata_hdr);
    int count = 0;
    for (;;) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        if (h->type == BTAG_DEVICE) {
            struct btag_device *d = (struct btag_device *)p;
            if (d->cls == cls) {
                if (count == nth)
                    return d;
                count++;
            }
        }
        p += h->size;
    }
}

#endif /* BOOTDATA_H */

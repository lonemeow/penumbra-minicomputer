/* bootdata.c — device discovery from the ROM's boot data. */

#include "rt_internal.h"

/* Mirrors hw/rom/bootdata.h. */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2

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

int rt_find_device(uint32_t bootdata, uint32_t cls,
                   uint32_t *base, uint32_t *size)
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
            if (d->cls == cls) {
                *base = d->base;
                if (size)
                    *size = d->dev_size;
                return 1;
            }
        }
        p += h->size;
    }
}

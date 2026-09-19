/*
 * memory.c — the heap: RAM above the stack, in blocks with a free list
 * kept in address order so neighbours can be merged back together.
 *
 * The ROM loads the image low, directly above its copy of the ELF file;
 * crt0.S starts the stack at STACK_TOP growing down.  Everything from
 * STACK_TOP to the end of base RAM is free.
 *
 * Parts hold buffers for as long as they run and hand them back from
 * leave(), by which time the next part has already allocated its own,
 * so blocks are not released in the order they were taken.  Allocation
 * is not interrupt-safe; the replayer never allocates.
 */

#include "rt_internal.h"

#define STACK_TOP       0x00100000      /* matches crt0.S */
#define STACK_RESERVE   0x00010000

#define ALIGN           16              /* one L2 line */

#define BLK_BUSY        0x55534544u     /* "USED" */
#define BLK_FREE        0x46524545u     /* "FREE" */

struct blk {
    uint32_t    size;                   /* bytes, this header included */
    uint32_t    state;
    struct blk *next;                   /* free list, by address */
};

/* The header rounded up to ALIGN, so payloads start on a line too. */
#define BLK_HDR  ((sizeof(struct blk) + ALIGN - 1) & ~(ALIGN - 1u))

extern char _end[];

static struct blk *free_list;
static uint32_t heap_end;

void rt_mem_init(uint32_t bootdata)
{
    uint32_t base, size;

    if (!rt_find_device(bootdata, ACFG_CLASS_MEMORY, &base, &size) || base)
        rt_panic("no base RAM in the boot data");
    if ((uint32_t)_end > STACK_TOP - STACK_RESERVE)
        rt_panic("image runs into the stack");

    heap_end = size;
    free_list = (struct blk *)STACK_TOP;
    free_list->size = heap_end - STACK_TOP;
    free_list->state = BLK_FREE;
    free_list->next = 0;
}

void *rt_alloc(uint32_t bytes)
{
    uint32_t want = (bytes + BLK_HDR + ALIGN - 1) & ~(ALIGN - 1u);
    struct blk *b, **link;

    if (want < bytes)
        rt_panic("allocation too large");

    /* First fit: parts take a few large buffers, so the list stays short
     * and a best fit would walk it for the same block. */
    for (link = &free_list; (b = *link) != 0; link = &b->next) {
        if (b->size < want)
            continue;

        /* A remainder too small to carry a header of its own stays with
         * the allocation. */
        if (b->size - want >= BLK_HDR + ALIGN) {
            struct blk *rest = (struct blk *)((char *)b + want);

            rest->size = b->size - want;
            rest->state = BLK_FREE;
            rest->next = b->next;
            b->size = want;
            *link = rest;
        } else {
            *link = b->next;
        }

        b->state = BLK_BUSY;
        b->next = 0;
        return (char *)b + BLK_HDR;
    }

    rt_panic("out of memory");
}

/* Absorbs second into first when the two are contiguous. */
static void merge(struct blk *first, struct blk *second)
{
    if (second && (char *)first + first->size == (char *)second) {
        first->size += second->size;
        first->next = second->next;
    }
}

void rt_free(void *p)
{
    struct blk *b, *prev = 0, *above = free_list;

    if (!p)
        return;

    b = (struct blk *)((char *)p - BLK_HDR);
    if (b->state != BLK_BUSY)
        rt_panic("freeing something that is not a live allocation");
    b->state = BLK_FREE;

    while (above && above < b) {
        prev = above;
        above = above->next;
    }

    b->next = above;
    if (prev)
        prev->next = b;
    else
        free_list = b;

    /* Upward first: prev then absorbs whatever b has become. */
    merge(b, above);
    if (prev)
        merge(prev, b);
}

void rt_mem_stats(uint32_t *total, uint32_t *largest)
{
    uint32_t t = 0, m = 0;

    for (struct blk *b = free_list; b; b = b->next) {
        t += b->size;
        if (b->size > m)
            m = b->size;
    }
    if (total)
        *total = t;
    if (largest)
        *largest = m > BLK_HDR ? m - BLK_HDR : 0;
}

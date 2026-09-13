/*
 * memory.c — bump allocation from the RAM above the stack.
 *
 * The ROM loads the image low, directly above its copy of the ELF file;
 * crt0.S starts the stack at STACK_TOP growing down.  Everything from
 * STACK_TOP to the end of base RAM is free.
 */

#include "rt_internal.h"

#define STACK_TOP       0x00100000      /* matches crt0.S */
#define STACK_RESERVE   0x00010000

extern char _end[];

static uint32_t next, limit;

void rt_mem_init(uint32_t bootdata)
{
    uint32_t base;

    if (!rt_find_device(bootdata, ACFG_CLASS_MEMORY, &base, &limit) || base)
        rt_panic("no base RAM in the boot data");
    if ((uint32_t)_end > STACK_TOP - STACK_RESERVE)
        rt_panic("image runs into the stack");
    next = STACK_TOP;
}

void *rt_alloc(uint32_t bytes)
{
    uint32_t p = next;

    bytes = (bytes + 3) & ~3u;
    if (bytes > limit - p)
        rt_panic("out of memory");
    next = p + bytes;
    return (void *)p;
}

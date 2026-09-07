/*
 * tlbrange.c — cacheable-range override for the bare-metal TLB miss
 * handler.
 *
 * Separate from bench.c because it is a service of one particular
 * startup: it reaches into the handler crt0.S copies to page 0x1000
 * and depends on that handler's scratch symbols.  Dhrystone links its
 * own startup with a different handler, so a benchmark opts into this
 * by linking the object, not by including a header.
 */

#include "bench.h"

/* ── Stringification for inline asm sysreg operands ────────────── */
#define _STR(x)  #x
#define XSTR(x)  _STR(x)

#define write_sysreg(dev, reg, val) do {                             \
    uint32_t __v = (val);                                            \
    __asm__ __volatile__("wrsys %0, " XSTR(dev) ", " XSTR(reg)       \
                         :: "r"(__v));                               \
} while (0)

/* ── MMU sysregs (device 0) ────────────────────────────────────── */
#define SYSDEV_MMU      0
#define MMU_TLB_PTE     4
#define MMU_TLB_INDEX   5

/* Main TLB is 32 sets x 2 ways.  The pinned TLB is selected by bit 6
 * of TLB_INDEX and is deliberately left alone — it holds the handler
 * and vector pages, which no caller's range covers. */
#define TLB_MAIN_SLOTS  64

/* Where crt0.S parks the handler block, and so where the copy of its
 * range words the running handler actually reads lives. */
#define HANDLER_PAGE    0x1000

/*
 * The handler block is copied to HANDLER_PAGE at startup and reaches
 * its own scratch PC-relatively, so the words the running handler
 * consults are in the copy, not in the linked image.  The offset
 * between the two symbols is fixed at link time and survives the copy,
 * which is what makes the copy's address computable.
 */
extern char _handler_start[];
extern char _rmh_cached_base[];

void bench_map_cached(uint32_t base, uint32_t limit)
{
    volatile uint32_t *range = (volatile uint32_t *)
        (HANDLER_PAGE + (uint32_t)(_rmh_cached_base - _handler_start));

    range[0] = base;
    range[1] = limit;

    /* Invalidating the slot covering this loop's own code is safe: the
     * fetch that follows misses, and the pinned handler refills it. */
    for (uint32_t i = 0; i < TLB_MAIN_SLOTS; i++) {
        write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, i);
        write_sysreg(SYSDEV_MMU, MMU_TLB_PTE, 0);
    }
}

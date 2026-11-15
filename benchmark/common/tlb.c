/*
 * tlb.c — the TLB services a program uses once bench_mmu_start (mmu.S)
 * has brought translation up (declared in mmu.h).
 *
 * The geometry and version come from the words bench_mmu_start writes
 * into the handler block's copy on HANDLER_PAGE, so MMU_CONFIG is
 * decoded in one place.  The copy is reached by each word's offset in
 * the linked block, a link-time constant that survives the copy.
 */

#include "bench.h"

/* ── Sysreg access: RDSYS/WRSYS take their operands as immediates ─ */
#define _STR(x)  #x
#define XSTR(x)  _STR(x)

#define write_sysreg(dev, reg, val) do {                             \
    uint32_t __v = (val);                                            \
    __asm__ __volatile__("wrsys %0, " XSTR(dev) ", " XSTR(reg)       \
                         :: "r"(__v));                               \
} while (0)

#define read_sysreg(dev, reg) ({                                     \
    uint32_t __v;                                                    \
    __asm__ __volatile__("rdsys %0, " XSTR(dev) ", " XSTR(reg)       \
                         : "=r"(__v));                               \
    __v;                                                             \
})

#define SYSDEV_MMU          0
#define MMU_TLB_VPN         3
#define MMU_TLB_PTE         4
#define MMU_TLB_INDEX       5
#define MMU_TLB_INVAL_ALL   8

extern char _handler_start[];
extern char _rmh_set_mask[], _rmh_way_stride[], _rmh_main_slots[];
extern char _rmh_version[], _rmh_pin_sel[], _rmh_pinned[];
extern char _rmh_cached_base[];

#define COPY(sym) ((volatile uint32_t *)                              \
    (HANDLER_PAGE + (uint32_t)((sym) - _handler_start)))

void bench_tlb_invalidate_all(void)
{
    if (*COPY(_rmh_version) == 2) {
        write_sysreg(SYSDEV_MMU, MMU_TLB_INVAL_ALL, 0);
        return;
    }

    /* Invalidating the slot covering this loop's own code is safe: the
     * fetch that follows misses, and the pinned handler refills it. */
    uint32_t slots = *COPY(_rmh_main_slots);
    for (uint32_t i = 0; i < slots; i++) {
        write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, i);
        write_sysreg(SYSDEV_MMU, MMU_TLB_PTE, 0);
    }
}

void bench_tlb_pin(unsigned slot, uint32_t va, uint32_t pte)
{
    if (slot < BENCH_PIN_FIRST_FREE || slot >= *COPY(_rmh_pinned)) {
        bench_puts("bench_tlb_pin: no pinned slot ");
        bench_print_uint(slot);
        bench_puts("\r\n");
        __asm__ __volatile__("break");
    }

    write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, *COPY(_rmh_pin_sel) | slot);
    write_sysreg(SYSDEV_MMU, MMU_TLB_VPN, (va >> 12) << 8);    /* ASID 0 */
    write_sysreg(SYSDEV_MMU, MMU_TLB_PTE, pte);
}

uint32_t bench_tlb_lookup(uint32_t va)
{
    uint32_t vpn    = va >> 12;
    uint32_t set    = vpn & *COPY(_rmh_set_mask);
    uint32_t stride = *COPY(_rmh_way_stride);
    uint32_t slots  = *COPY(_rmh_main_slots);

    /* An invalidated entry keeps its VPN word, so only V says whether a
     * matching tag is resident. */
    for (uint32_t slot = set; slot < slots; slot += stride) {
        write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, slot);
        if ((read_sysreg(SYSDEV_MMU, MMU_TLB_VPN) >> 8) != vpn)
            continue;
        uint32_t pte = read_sysreg(SYSDEV_MMU, MMU_TLB_PTE);
        if (pte & PTE_V)
            return pte;
    }
    return 0;
}

void bench_tlb_geom(uint32_t *sets, uint32_t *ways)
{
    *sets = *COPY(_rmh_set_mask) + 1u;
    *ways = *COPY(_rmh_main_slots) / *sets;
}

void bench_map_cached(uint32_t base, uint32_t limit)
{
    volatile uint32_t *range = COPY(_rmh_cached_base);

    range[0] = base;
    range[1] = limit;
    bench_tlb_invalidate_all();
}

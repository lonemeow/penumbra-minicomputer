/*
 * mmu.h — the bare-metal harness's MMU, for any MMU_CONFIG version and
 * geometry (doc/system/mmu.md).
 *
 * Both startups bring translation up through bench_mmu_start (mmu.S):
 * the TLB initialized, the exception handlers on HANDLER_PAGE, that
 * page and page 0 pinned, the MMU and the caches on.  From then on the
 * miss handler maps every page on demand:
 *
 *   VA >= 0xFF000000   identity, uncached (devices), unless inside the
 *                      range bench_map_cached() names
 *   VA >= 0x80000000   cached, PA = VA + load bias (a static image's
 *                      window; the bias is 0 for a PIE image)
 *   otherwise          identity, cached
 *
 * The handlers keep their state in SCR0-SCR3, which a program leaves
 * alone.  The defines are shared with mmu.S; the declarations are
 * tlb.c's.
 */

#ifndef BENCH_MMU_H
#define BENCH_MMU_H

/* The page the handler block is copied to, pinned with page 0 */
#define HANDLER_PAGE            0x1000

/* Pinned slots: the harness's two, then the program's */
#define BENCH_PIN_HANDLER       0
#define BENCH_PIN_PAGE0         1
#define BENCH_PIN_FIRST_FREE    2

/* Where a static image's window starts; must match bench_static.ld */
#define BENCH_VBASE             0x80000000

/* PTE flags, and the flag words the handler installs */
#define PTE_V                   0x01
#define PTE_C                   0x04
#define PTE_KERNEL              0xBD    /* V|C|R|W|X|G */
#define PTE_KERNEL_NC           0x99    /* V|R|W|G: uncached, for devices */

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * Invalidate every main-TLB entry: TLB_INVAL_ALL in version 2, slot by
 * slot in version 1.  The pinned TLB is left alone.  Safe with the MMU
 * on: the next access misses, and the pinned handler refills it.
 */
void bench_tlb_invalidate_all(void);

/*
 * Pin `va`'s page in pinned slot `slot` with PTE word `pte` (PA page
 * base | flags).  Slots below BENCH_PIN_FIRST_FREE are the harness's;
 * a slot outside [BENCH_PIN_FIRST_FREE, PINNED) stops the program.
 */
void bench_tlb_pin(unsigned slot, uint32_t va, uint32_t pte);

/*
 * The PTE word of the valid main-TLB entry for `va`'s page, or 0 when
 * none is resident: what the miss handler decided for it.
 */
uint32_t bench_tlb_lookup(uint32_t va);

/*
 * Map [base, limit) cached even where it falls in the device range the
 * miss handler otherwise maps uncached.  For a device aperture that
 * behaves as plain memory — a framebuffer whose only writer is the CPU
 * — where a cached copy cannot go stale; never for registers the
 * device itself updates.
 *
 * Flushes the main TLB, since entries installed under the previous
 * range still carry its cacheability.  Pass (0, 0) to restore the
 * default, under which every device is uncached.
 */
void bench_map_cached(uint32_t base, uint32_t limit);

#endif /* !__ASSEMBLER__ */

#endif /* BENCH_MMU_H */

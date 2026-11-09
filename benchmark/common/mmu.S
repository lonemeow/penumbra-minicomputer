/*
 * mmu.S — the bare-metal harness's translation set-up and exception
 * handlers, for any MMU_CONFIG version and geometry (mmu.h).
 *
 * Both startups call bench_mmu_start once, with the MMU off and no
 * stack:
 *   R1  runtime address of _handler_start
 *   R2  load bias, added to a VA at or above BENCH_VBASE for its PA
 *       (0 for a PIE image, which runs where it was loaded)
 * It initializes the TLB, whose contents are undefined after reset;
 * copies the handler block to HANDLER_PAGE and points the vectors at
 * it; pins that page and page 0; turns translation and the caches on;
 * and returns.  R5 is kept; R1-R4, R6-R12 and the flags are not.
 *
 * The handlers own SCR0-SCR3: SCR0-SCR2 hold the registers they free,
 * and SCR3 the way the next refill takes, times SETS.
 *
 * Translation comes up by Pattern B (doc/system/mmu.md, "Enabling the
 * MMU Safely"): the fetch after MMUCR.M is set misses, and the pinned
 * miss handler maps it.  A static image is still running at its
 * physical address then, below BENCH_VBASE, so the handler maps it
 * identically, as it does a PIE image throughout.
 */

#include "mmu.h"

/* ── Sysregs ─────────────────────────────────────────────────── */
#define SYSDEV_MMU          0
#define MMU_MMUCR           0
#define MMU_FAULT_ADDR      1
#define MMU_TLB_VPN         3
#define MMU_TLB_PTE         4
#define MMU_TLB_INDEX       5
#define MMU_TLB_INVAL_ALL   8
#define MMU_CONFIG          15
#define MMUCR_M             1

#define SYSDEV_L1_DCACHE    2
#define SYSDEV_L1_ICACHE    3
#define SYSDEV_TIMER        7
#define SYSDEV_L2_CACHE     9
#define CACHE_INFO          0
#define CACHE_CTRL          1
#define CACHE_CTRL_ENABLE   1
#define TM_STATUS           4

/* ── MMU_CONFIG fields ───────────────────────────────────────── */
#define MMUCFG_VER_MASK         0x0F
#define MMUCFG_PINNED_SHIFT     4       /* 6 bits */
#define MMUCFG_SETS_LOG2_SHIFT  10      /* 5 bits */
#define MMUCFG_WAYS_SHIFT       15      /* 5 bits */

/* What a read of zero reports: version 1, 8 pinned, 32 sets of 2 ways */
#define MMUCFG_V1_BASELINE      0x10C11481

/* TLB_INDEX's pinned select, by version */
#define TLB_PIN_SEL_V1          0x40
#define TLB_PIN_SEL_V2          0x80000000

/* Page 0: accumulated timer ticks, beside the vectors and boot data */
#define TIMER_BASE_ADDR         0x38

/* A word of the handler block's copy on HANDLER_PAGE */
#define COPY(sym)               (HANDLER_PAGE + ((sym) - _handler_start))


        .text
        .globl  bench_mmu_start
        .type   bench_mmu_start,@function
bench_mmu_start:
        mov     r10, r1                 /* runtime &_handler_start */
        mov     r11, r2                 /* load bias */

        /* R6 = MMU_CONFIG */
        rdsys   r6, #SYSDEV_MMU, #MMU_CONFIG
        cmp     r6, r0
        bne     .Lcfg
        li      r6, MMUCFG_V1_BASELINE
.Lcfg:
        /* R7 = SETS, the stride between ways; R8 = SETS * WAYS */
        mov     r1, r6
        shr     r1, #MMUCFG_SETS_LOG2_SHIFT
        and     r1, #0x1F
        lli     r7, #1
        shl     r7, r1
        mov     r8, r6
        shr     r8, #MMUCFG_WAYS_SHIFT
        and     r8, #0x1F

        /* One way cannot hold an instruction's page and its data's when
         * the two share a set: each refill evicts the other, and the
         * instruction never completes.  Two can, since consecutive
         * refills take different ways.  Stop rather than spin. */
        cmp     r8, #2
        bhs     .Lways_ok
        break
.Lways_ok:
        shl     r8, r1

        /* Invalidate the main TLB the version's way; R9 = TLB_INDEX's
         * pinned select.  A version this harness does not know stops
         * here rather than guess at its registers. */
        mov     r1, r6
        and     r1, #MMUCFG_VER_MASK
        cmp     r1, #2
        beq     .Lv2
        cmp     r1, #1
        beq     .Lv1
        break
.Lv1:
        lli     r9, #TLB_PIN_SEL_V1
        mov     r1, r0
.Lv1_slot:
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX
        wrsys   r0, #SYSDEV_MMU, #MMU_TLB_PTE
        add     r1, #1
        cmp     r1, r8
        blo     .Lv1_slot
        b       .Lpinned
.Lv2:
        li      r9, TLB_PIN_SEL_V2
        wrsys   r0, #SYSDEV_MMU, #MMU_TLB_INVAL_ALL

        /* Write every pinned slot, V=0: a pinned hit wins over the main
         * TLB, so an entry left in one would capture its page.  R12 =
         * PINNED. */
.Lpinned:
        mov     r12, r6
        shr     r12, #MMUCFG_PINNED_SHIFT
        and     r12, #0x3F
        mov     r1, r0
.Lpin_clear:
        mov     r2, r1
        or      r2, r9
        wrsys   r2, #SYSDEV_MMU, #MMU_TLB_INDEX
        wrsys   r0, #SYSDEV_MMU, #MMU_TLB_PTE
        add     r1, #1
        cmp     r1, r12
        blo     .Lpin_clear

        /* Copy the handler block, then give the copy its translation
         * state.  The block reaches its scratch PC-relatively, so it
         * runs wherever it lands. */
        stw     r0, [r0 + TIMER_BASE_ADDR]
        li      r1, HANDLER_PAGE
        li      r2, _handler_end - _handler_start
.Lcopy:
        ldw     r3, [r10]
        stw     r3, [r1]
        add     r10, #4
        add     r1, #4
        sub     r2, #4
        bne     .Lcopy

        li      r1, COPY(_rmh_load_bias)
        stw     r11, [r1]
        li      r1, COPY(_rmh_set_mask)
        mov     r2, r7
        sub     r2, #1
        stw     r2, [r1]
        li      r1, COPY(_rmh_way_stride)
        stw     r7, [r1]
        li      r1, COPY(_rmh_main_slots)
        stw     r8, [r1]
        li      r1, COPY(_rmh_version)
        mov     r2, r6
        and     r2, #MMUCFG_VER_MASK
        stw     r2, [r1]
        li      r1, COPY(_rmh_pin_sel)
        stw     r9, [r1]
        li      r1, COPY(_rmh_pinned)
        stw     r12, [r1]
        wrspr   scr3, r0                /* the first refill takes way 0 */

        li      r1, COPY(_bench_tlb_miss)
        stw     r1, [r0 + 8]            /* vector[2] = TLB miss */
        li      r1, COPY(_bench_irq)
        stw     r1, [r0 + 4]            /* vector[1] = IRQ */

        /* Pin the handler page and page 0 (vectors, boot data, timer
         * ticks), identity-mapped, so no handler ever misses */
        mov     r1, r9
        add     r1, #BENCH_PIN_HANDLER
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX
        lli     r1, #(HANDLER_PAGE >> 4)            /* VPN << 8, ASID 0 */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_VPN
        lli     r1, #(HANDLER_PAGE | PTE_KERNEL)
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        mov     r1, r9
        add     r1, #BENCH_PIN_PAGE0
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX
        wrsys   r0, #SYSDEV_MMU, #MMU_TLB_VPN
        lli     r1, #PTE_KERNEL
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        lli     r1, #MMUCR_M
        wrsys   r1, #SYSDEV_MMU, #MMU_MMUCR

        /* Caches on.  PTE.C only declares a page cacheable; each cache
         * passes everything through until its ENABLE is set.  An L2
         * reading INFO = 0 is absent, and takes no write. */
        lli     r1, #CACHE_CTRL_ENABLE
        wrsys   r1, #SYSDEV_L1_DCACHE, #CACHE_CTRL
        wrsys   r1, #SYSDEV_L1_ICACHE, #CACHE_CTRL
        rdsys   r2, #SYSDEV_L2_CACHE, #CACHE_INFO
        cmp     r2, r0
        beq     .Lno_l2
        wrsys   r1, #SYSDEV_L2_CACHE, #CACHE_CTRL
.Lno_l2:
        ret

        .size   bench_mmu_start, . - bench_mmu_start


/* ═════════════════════════════════════════════════════════════════
 * Handler block — copied to HANDLER_PAGE by bench_mmu_start.
 *
 * Self-contained: the handlers park registers in the scratch SPRs and
 * reach their state PC-relatively, so the copy runs at its own address.
 * Symbols here name the linked image; the running words are the copy's
 * (COPY() above, and tlb.c).
 * ═════════════════════════════════════════════════════════════════ */

        .section .handlers,"ax",@progbits
        .balign 4
        .globl  _handler_start
        .globl  _handler_end

_handler_start:

/* ── TLB miss handler ──────────────────────────────────────────
 *
 * Maps the faulting page by its range (mmu.h) into its set, the ways
 * taken in turn.  Attribute bits are added rather than or'd: OR has no
 * immediate form, and the page base leaves the low twelve bits clear,
 * where the two are the same operation.
 * ────────────────────────────────────────────────────────────── */
        .globl  _bench_tlb_miss
_bench_tlb_miss:
        wrspr   scr0, r1
        wrspr   scr1, r2
        wrspr   scr2, r3

        rdsys   r2, #SYSDEV_MMU, #MMU_FAULT_ADDR
        shr     r2, #12                         /* R2 = VPN */

        /* TLB_INDEX = the VPN's set in SCR3's way, then SCR3 steps to
         * the next way, back to way 0 after the last */
        rdspr   r1, scr3                        /* way * SETS */
        ldw     r3, [pc + _rmh_set_mask - .]
        and     r3, r2
        add     r3, r1
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_INDEX
        ldw     r3, [pc + _rmh_way_stride - .]
        add     r1, r3
        ldw     r3, [pc + _rmh_main_slots - .]
        cmp     r1, r3
        blo     .Lnext_way
        mov     r1, r0
.Lnext_way:
        wrspr   scr3, r1

        /* TLB_VPN = VPN << 8, ASID 0 */
        mov     r3, r2
        shl     r3, #8
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_VPN

        /* TLB_PTE, by VA[31:24]: R3 = the page base, R1 the top byte */
        mov     r3, r2
        shl     r3, #12
        mov     r1, r2
        shr     r1, #12
        cmp     r1, #0xFF
        bhs     .Ldevice
        cmp     r1, #(BENCH_VBASE >> 24)
        blo     .Lcached
        ldw     r1, [pc + _rmh_load_bias - .]   /* page-aligned */
        add     r3, r1
.Lcached:
        add     r3, #PTE_KERNEL
        b       .Lcommit

        /* A device page is uncached unless it is in [base, limit),
         * compared unsigned and at page granularity: the bounds only
         * ever select whole pages.  Both zero, as at start, no page
         * is. */
.Ldevice:
        ldw     r1, [pc + _rmh_cached_base - .]
        cmp     r3, r1
        blo     .Luncached
        ldw     r1, [pc + _rmh_cached_limit - .]
        cmp     r3, r1
        bhs     .Luncached
        add     r3, #PTE_KERNEL
        b       .Lcommit
.Luncached:
        add     r3, #PTE_KERNEL_NC
.Lcommit:
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_PTE

        rdspr   r1, scr0
        rdspr   r2, scr1
        rdspr   r3, scr2
        eret


/* ── IRQ handler: timer tick accumulation ──────────────────────
 *
 * On timer underflow, clears the UDF flag and adds one full period
 * (0xFFFF ticks, matching TMRELOAD = 0xFFFE) to the tick count at
 * TIMER_BASE_ADDR, so reading the time needs no multiply.  ERET
 * restores the flags from ESR.
 *
 * R1 shares SCR0 with the miss handler: neither can run inside the
 * other, since exception entry masks the IRQ and this handler touches
 * only pinned pages.
 * ────────────────────────────────────────────────────────────── */
        .balign 4
        .globl  _bench_irq
_bench_irq:
        wrspr   scr0, r1

        lli     r1, #1
        wrsys   r1, #SYSDEV_TIMER, #TM_STATUS   /* write-1-to-clear */

        ldw     r1, [r0 + TIMER_BASE_ADDR]
        add     r1, #0xFFFF
        stw     r1, [r0 + TIMER_BASE_ADDR]

        rdspr   r1, scr0
        eret


/* ── Translation state (contiguous with the code for PC-relative
 *    access), written into the copy by bench_mmu_start and read by
 *    tlb.c through it ── */
        .balign 4
        .globl  _rmh_load_bias, _rmh_set_mask, _rmh_way_stride
        .globl  _rmh_main_slots, _rmh_version
        .globl  _rmh_pin_sel, _rmh_pinned
_rmh_load_bias:  .long 0        /* PA - VA at or above BENCH_VBASE */
_rmh_set_mask:   .long 0        /* SETS - 1 */
_rmh_way_stride: .long 0        /* SETS: one way's slots */
_rmh_main_slots: .long 0        /* SETS * WAYS */
_rmh_version:    .long 0        /* MMU_CONFIG's VER */
_rmh_pin_sel:    .long 0        /* TLB_INDEX's pinned select */
_rmh_pinned:     .long 0        /* PINNED */

/* Cacheable device range, adjacent so tlb.c writes the pair as two
 * words from one base */
        .globl  _rmh_cached_base, _rmh_cached_limit
_rmh_cached_base:  .long 0
_rmh_cached_limit: .long 0

_handler_end:

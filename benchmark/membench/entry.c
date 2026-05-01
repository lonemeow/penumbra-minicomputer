/*
 * entry.c — bench_main for the memory throughput / latency bench.
 *
 * Test matrix:
 *
 *                 word (LDW/STW)   half (LDH/STH)   byte (LDB/STB)
 *   cached read        ✓                ✓                 ✓
 *   cached write       ✓                ✓                 ✓
 *   uncached read      ✓                ✓                 ✓
 *   uncached write     ✓                ✓                 ✓
 *
 * Cached tests sweep a 64 KiB working set — comfortably larger than
 * the 1 KiB direct-mapped D-cache (64 sets × 4 words × 1 way × 4 B).
 * Sequential access pattern → 75% cache hits + 25% line-fill misses,
 * so the cached MB/s number reflects the *burst-fill* path through
 * cache → bus_adapter → CDC → controller.  Open-row tracking (step 6
 * of the SDRAM rollout) should improve this number directly because
 * sequential cache lines all hit the same row.
 *
 * Uncached tests pin a single 4 KiB page with PTE_KERNEL_NC and run
 * a tight loop within it.  Every access becomes a single-word SDRAM
 * round-trip — the ratio between this and the cached number is the
 * controller's burst speedup.  Step 6 won't change the uncached
 * number (each access still pays the full ACT/RW/PRECHARGE cost).
 *
 * Cache hit/miss microbenchmarks isolate the two endpoints of the
 * cached-sweep curve:
 *   • Cache hit: HIT_WORKING_BYTES (512 B) fits inside the 1 KiB
 *     D-cache, so after the first warmup pass every access hits.
 *     Reports MB/s — the upper bound of cached throughput.
 *   • Cache miss: stride MISS_STRIDE_BYTES (64 B) exceeds any
 *     plausible cacheline, so every access pays a full line-fill.
 *     Reports ns/miss — isolates SDRAM-roundtrip + bus-traversal
 *     latency for one cache line.  Direct measurement of the path
 *     that pipelined SDRAM reads should accelerate.
 *
 * Each test runs for ~MEAS_TARGET_US of measured time and reports
 * MB/s (throughput) or ns/op (latency) so the numbers are directly
 * comparable across architectures and across optimisation commits.
 */

#include "bench.h"

/* ── Bootdata layout (mirrored, see hw/rom/bootdata.h) ─────────── */
#define BOOTDATA_BASE        0x00000040
#define BTAG_END             0
#define BTAG_DEVICE          2
#define ACFG_CLASS_MEMORY    1

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

/* ── Sysreg numbers (mirrors crt0.S) ──────────────────────────── */
#define _STR(x)  #x
#define XSTR(x)  _STR(x)

#define SYSDEV_MMU       0
#define MMU_TLB_VPN      3
#define MMU_TLB_PTE      4
#define MMU_TLB_INDEX    5
#define PTE_KERNEL_NC    0x99u
#define TLB_PINNED       0x40u

#define write_sysreg(dev, reg, val) do {                         \
    uint32_t _v = (val);                                         \
    __asm__ __volatile__("wrsys %0, " XSTR(dev) ", " XSTR(reg)   \
                         : : "r"(_v));                           \
} while (0)

/* ── Test geometry ────────────────────────────────────────────── */
#define CACHED_WORKING_BYTES    (64u * 1024u)   /* 64× the 1 KiB cache */
#define HIT_WORKING_BYTES       (512u)          /* fits inside 1 KiB D-cache */
#define MISS_STRIDE_BYTES       64u             /* > any plausible cacheline */
#define UNCACHED_PAGE_BYTES     (4u  * 1024u)
#define UNCACHED_TEST_VA        0x80000000u     /* unused VA range */
#define MEMTEST_RESERVED_LO     (2u  * 1024u * 1024u)

/* Each test loops until the timer crosses MEAS_TARGET_US.  Long
 * enough to amortize timer-readback noise; short enough that the
 * full 12-test matrix finishes in a few seconds. */
#define MEAS_TARGET_US          200000u   /* 200 ms */

/* ── Bootdata RAM lookup ──────────────────────────────────────── */
static const struct btag_device *find_first_memory_region(uint32_t bootdata) {
    uint32_t p = (bootdata ? bootdata : BOOTDATA_BASE) + 12;
    for (;;) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        if (h->type == BTAG_DEVICE) {
            struct btag_device *d = (struct btag_device *)p;
            if (d->cls == ACFG_CLASS_MEMORY)
                return d;
        }
        p += h->size;
    }
}

/* ── Pin a TLB slot with explicit PTE flags ───────────────────── */
/*
 * Slots 0 and 1 are taken by the bench harness (handler page +
 * page 0).  Slot 2 is the next free pinned slot — we use it for
 * the uncached test page.
 */
static void pin_uncached(unsigned slot, uint32_t va, uint32_t pa) {
    uint32_t idx = TLB_PINNED | (slot & 0x3);
    uint32_t vpn = (va >> 12);
    uint32_t pte = (pa & ~0xFFFu) | PTE_KERNEL_NC;
    write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, idx);
    write_sysreg(SYSDEV_MMU, MMU_TLB_VPN,   vpn << 8);   /* ASID = 0 */
    write_sysreg(SYSDEV_MMU, MMU_TLB_PTE,   pte);
}

/* ── Hex print helper ─────────────────────────────────────────── */
static void print_hex32(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    bench_putchar('0'); bench_putchar('x');
    for (int s = 28; s >= 0; s -= 4)
        bench_putchar(digits[(v >> s) & 0xF]);
}

/* ── Throughput print helper ──────────────────────────────────────
 * Print bytes/µs as "X.YYY MB/s" so sub-MB/s rates don't round to 0.
 * Internal computation uses kB/s = bytes / (us/1000) which keeps the
 * intermediate within 32 bits even for hundreds of MB total bytes.
 * (Required ranges: us ≥ 1000 — guaranteed by MEAS_TARGET_US = 200 ms.)
 */
static void print_mb_per_s(uint32_t bytes, uint32_t us) {
    uint32_t kb_per_s = bytes / (us / 1000);
    uint32_t mb_int   = kb_per_s / 1000;
    uint32_t mb_frac  = kb_per_s % 1000;
    bench_print_uint(mb_int);
    bench_putchar('.');
    if (mb_frac < 100) bench_putchar('0');
    if (mb_frac <  10) bench_putchar('0');
    bench_print_uint(mb_frac);
    bench_puts(" MB/s");
}

/* ── Cached sweep loops ────────────────────────────────────────
 * Each pass walks the full working set, then we count how many
 * passes fit in MEAS_TARGET_US.  Total bytes = passes × working_set.
 *
 * Inner loops are manually unrolled 8× because the accesses are
 * volatile (so clang -O2 leaves them alone): a tight 1-op-per-iter
 * loop spends ~80% of its time on loop control + ALU bookkeeping,
 * which masks any improvement to the memory subsystem.  With 8×
 * unroll the per-access overhead drops to ~0.6 instructions and
 * the measurement starts to reflect what the cache + SDRAM stack
 * can actually deliver.  Working-set sizes (64 KiB cached, 4 KiB
 * uncached) are all multiples of 32 bytes so the unrolled stride
 * lands cleanly with no tail loop.
 */

typedef uint32_t (*sweep_fn)(uint32_t base, uint32_t bytes);

/* Returned dummy is XOR'd with bench_sink so the compiler can't
 * elide the load chain. */
static uint32_t sweep_ldw(uint32_t base, uint32_t bytes) {
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint32_t n = bytes >> 2;
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i += 8) {
        s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
        s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
    }
    return s;
}

static uint32_t sweep_ldh(uint32_t base, uint32_t bytes) {
    volatile unsigned short *p = (volatile unsigned short *)base;
    uint32_t n = bytes >> 1;
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i += 8) {
        s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
        s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
    }
    return s;
}

static uint32_t sweep_ldb(uint32_t base, uint32_t bytes) {
    volatile unsigned char *p = (volatile unsigned char *)base;
    uint32_t s = 0;
    for (uint32_t i = 0; i < bytes; i += 8) {
        s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
        s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
    }
    return s;
}

/* Constant store value — keeps the inner loop down to "STW + offset"
 * with the value held in one register; using `i` would force an ADD
 * per store. */
#define STORE_W   0xCAFEBABEu
#define STORE_H   0xCAFEu
#define STORE_B   0xABu

static uint32_t sweep_stw(uint32_t base, uint32_t bytes) {
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint32_t n = bytes >> 2;
    for (uint32_t i = 0; i < n; i += 8) {
        p[i+0] = STORE_W; p[i+1] = STORE_W; p[i+2] = STORE_W; p[i+3] = STORE_W;
        p[i+4] = STORE_W; p[i+5] = STORE_W; p[i+6] = STORE_W; p[i+7] = STORE_W;
    }
    return 0;
}

static uint32_t sweep_sth(uint32_t base, uint32_t bytes) {
    volatile unsigned short *p = (volatile unsigned short *)base;
    uint32_t n = bytes >> 1;
    for (uint32_t i = 0; i < n; i += 8) {
        p[i+0] = STORE_H; p[i+1] = STORE_H; p[i+2] = STORE_H; p[i+3] = STORE_H;
        p[i+4] = STORE_H; p[i+5] = STORE_H; p[i+6] = STORE_H; p[i+7] = STORE_H;
    }
    return 0;
}

static uint32_t sweep_stb(uint32_t base, uint32_t bytes) {
    volatile unsigned char *p = (volatile unsigned char *)base;
    for (uint32_t i = 0; i < bytes; i += 8) {
        p[i+0] = STORE_B; p[i+1] = STORE_B; p[i+2] = STORE_B; p[i+3] = STORE_B;
        p[i+4] = STORE_B; p[i+5] = STORE_B; p[i+6] = STORE_B; p[i+7] = STORE_B;
    }
    return 0;
}

/* ── Cache-miss strided read ──────────────────────────────────────
 * Same overall shape as sweep_ldw, but accesses are spaced by
 * MISS_STRIDE_BYTES (= 64 B) so each LDW lands in a different
 * cache line and every access pays a full line-fill.  Working set
 * (CACHED_WORKING_BYTES = 64 KiB) is many times the cache, so even
 * after the first pass none of the lines we touch are still resident.
 *
 * Number of misses per pass = bytes / MISS_STRIDE_BYTES.
 */
static uint32_t sweep_miss_ldw(uint32_t base, uint32_t bytes) {
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint32_t n = bytes >> 2;   /* total words in region */
    uint32_t s = 0;
    /* Stride is 16 words (= 64 B); 8 accesses per outer iter cover 128 words. */
    for (uint32_t i = 0; i < n; i += 128) {
        s ^= p[i +  0]; s ^= p[i + 16]; s ^= p[i + 32]; s ^= p[i + 48];
        s ^= p[i + 64]; s ^= p[i + 80]; s ^= p[i + 96]; s ^= p[i + 112];
    }
    return s;
}

/* ── Unified perf-line printer ────────────────────────────────────
 * One report format for every test: "<count> <unit>, <ms> ms, X.YYY
 * MB/s, <ns> ns/op".  bytes = useful-data total (passes × working
 * for sweeps, misses × 4 for the miss test, ops × op_size for
 * uncached); ops = total individual accesses.  Both us×1000 and
 * passes×working fit in u32 at MEAS_TARGET_US = 0.2 s. */
static volatile uint32_t bench_sink;

static void print_perf_line(const char *label,
                            uint32_t count,
                            const char *count_unit,
                            uint32_t us,
                            uint32_t bytes,
                            uint32_t ops) {
    uint32_t ns_per_op = (us * 1000u) / ops;
    bench_puts("  ");
    bench_puts(label);
    bench_puts(": ");
    bench_print_uint(count);
    bench_putchar(' ');
    bench_puts(count_unit);
    bench_puts(", ");
    bench_print_uint(us / 1000);
    bench_puts(" ms, ");
    print_mb_per_s(bytes, us);
    bench_puts(", ");
    bench_print_uint(ns_per_op);
    bench_puts(" ns/op\n");
}

/* ── Run one sweep test ───────────────────────────────────────── */
static void run_sweep_test(const char *label,
                           sweep_fn fn,
                           uint32_t base,
                           uint32_t working,
                           uint32_t op_size) {
    uint32_t deadline = bench_us_to_ticks(MEAS_TARGET_US);
    uint32_t sink = 0;
    uint32_t passes = 0;

    bench_timer_start();
    while (bench_timer_elapsed_ticks() < deadline) {
        sink ^= fn(base, working);
        passes++;
    }
    uint32_t us = bench_timer_elapsed_us();
    bench_sink ^= sink;

    uint32_t bytes = passes * working;
    uint32_t ops   = passes * (working / op_size);
    print_perf_line(label, passes, "passes", us, bytes, ops);
}

/* ── Run one cache-miss test ───────────────────────────────────
 * Same pass-counting harness as run_sweep_test.  Useful-byte total
 * is misses × 4 (one word per stride), so the MB/s number reads as
 * "useful data delivered" — directly comparable to hit MB/s. */
static void run_miss_test(const char *label,
                          sweep_fn fn,
                          uint32_t base,
                          uint32_t working,
                          uint32_t stride_bytes) {
    uint32_t deadline = bench_us_to_ticks(MEAS_TARGET_US);
    uint32_t sink = 0;
    uint32_t passes = 0;

    bench_timer_start();
    while (bench_timer_elapsed_ticks() < deadline) {
        sink ^= fn(base, working);
        passes++;
    }
    uint32_t us = bench_timer_elapsed_us();
    bench_sink ^= sink;

    uint32_t misses = passes * (working / stride_bytes);
    uint32_t bytes  = misses * 4u;   /* one word delivered per miss */
    print_perf_line(label, misses, "misses", us, bytes, misses);
}

/* ── Uncached fixed-page latency loops ─────────────────────────
 * Loop a fixed number of accesses inside the pinned page, report
 * ns/op.  Same 8× unroll as the cached sweeps so the per-op
 * latency we report is closer to "true SDRAM round-trip time"
 * and not "loop overhead + latency".
 *
 * 4 KiB / 4 = 1024 word ops per pass; / 2 = 2048 half ops; full
 * 4096 byte ops.  All multiples of 8.
 */

#define UNCACHED_PASSES 1024u   /* tunable; finishes well within MEAS_TARGET_US */

static void report_uncached(const char *label, uint32_t ops,
                            uint32_t op_size, uint32_t us) {
    print_perf_line(label, ops, "ops", us, ops * op_size, ops);
}

static void run_uncached_ldw(uint32_t va) {
    volatile uint32_t *p = (volatile uint32_t *)va;
    uint32_t s = 0;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES / 4; i += 8) {
            s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
            s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
        }
    uint32_t us = bench_timer_elapsed_us();
    bench_sink ^= s;

    uint32_t ops = UNCACHED_PASSES * (UNCACHED_PAGE_BYTES / 4);
    report_uncached("uncached LDW", ops, 4, us);
}

static void run_uncached_ldh(uint32_t va) {
    volatile unsigned short *p = (volatile unsigned short *)va;
    uint32_t s = 0;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES / 2; i += 8) {
            s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
            s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
        }
    uint32_t us = bench_timer_elapsed_us();
    bench_sink ^= s;

    uint32_t ops = UNCACHED_PASSES * (UNCACHED_PAGE_BYTES / 2);
    report_uncached("uncached LDH", ops, 2, us);
}

static void run_uncached_ldb(uint32_t va) {
    volatile unsigned char *p = (volatile unsigned char *)va;
    uint32_t s = 0;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES; i += 8) {
            s ^= p[i+0]; s ^= p[i+1]; s ^= p[i+2]; s ^= p[i+3];
            s ^= p[i+4]; s ^= p[i+5]; s ^= p[i+6]; s ^= p[i+7];
        }
    uint32_t us = bench_timer_elapsed_us();
    bench_sink ^= s;

    uint32_t ops = UNCACHED_PASSES * UNCACHED_PAGE_BYTES;
    report_uncached("uncached LDB", ops, 1, us);
}

static void run_uncached_stw(uint32_t va) {
    volatile uint32_t *p = (volatile uint32_t *)va;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES / 4; i += 8) {
            p[i+0] = STORE_W; p[i+1] = STORE_W; p[i+2] = STORE_W; p[i+3] = STORE_W;
            p[i+4] = STORE_W; p[i+5] = STORE_W; p[i+6] = STORE_W; p[i+7] = STORE_W;
        }
    uint32_t us = bench_timer_elapsed_us();

    uint32_t ops = UNCACHED_PASSES * (UNCACHED_PAGE_BYTES / 4);
    report_uncached("uncached STW", ops, 4, us);
}

static void run_uncached_sth(uint32_t va) {
    volatile unsigned short *p = (volatile unsigned short *)va;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES / 2; i += 8) {
            p[i+0] = STORE_H; p[i+1] = STORE_H; p[i+2] = STORE_H; p[i+3] = STORE_H;
            p[i+4] = STORE_H; p[i+5] = STORE_H; p[i+6] = STORE_H; p[i+7] = STORE_H;
        }
    uint32_t us = bench_timer_elapsed_us();

    uint32_t ops = UNCACHED_PASSES * (UNCACHED_PAGE_BYTES / 2);
    report_uncached("uncached STH", ops, 2, us);
}

static void run_uncached_stb(uint32_t va) {
    volatile unsigned char *p = (volatile unsigned char *)va;
    bench_timer_start();
    for (uint32_t k = 0; k < UNCACHED_PASSES; k++)
        for (uint32_t i = 0; i < UNCACHED_PAGE_BYTES; i += 8) {
            p[i+0] = STORE_B; p[i+1] = STORE_B; p[i+2] = STORE_B; p[i+3] = STORE_B;
            p[i+4] = STORE_B; p[i+5] = STORE_B; p[i+6] = STORE_B; p[i+7] = STORE_B;
        }
    uint32_t us = bench_timer_elapsed_us();

    uint32_t ops = UNCACHED_PASSES * UNCACHED_PAGE_BYTES;
    report_uncached("uncached STB", ops, 1, us);
}

/* ── bench_main ───────────────────────────────────────────────── */
void bench_main(uint32_t bootdata) {
    bench_init();

    bench_puts("\n=== Penumbra membench ===\n");

    const struct btag_device *ram = find_first_memory_region(bootdata);
    if (!ram) {
        bench_puts("No ACFG_CLASS_MEMORY entry — abort.\n");
        return;
    }

    /* Layout in the test region:
     *   [reserved low ........................ MEMTEST_RESERVED_LO ]
     *   [cached working set, 64 KiB                                ]
     *   [uncached test page (PA), 4 KiB                            ]
     */
    uint32_t cached_base = (ram->base + MEMTEST_RESERVED_LO + 0xFFF) & ~0xFFFu;
    uint32_t uncached_pa = cached_base + CACHED_WORKING_BYTES;

    /* Sanity check: the test region must fit in the discovered RAM. */
    if (uncached_pa + UNCACHED_PAGE_BYTES > ram->base + ram->dev_size) {
        bench_puts("Insufficient RAM for working set — abort.\n");
        return;
    }

    pin_uncached(2, UNCACHED_TEST_VA, uncached_pa);

    bench_puts("Cached working set:   ");
    print_hex32(cached_base); bench_puts(" .. ");
    print_hex32(cached_base + CACHED_WORKING_BYTES); bench_puts("\n");
    bench_puts("Uncached page:        VA ");
    print_hex32(UNCACHED_TEST_VA); bench_puts(" -> PA ");
    print_hex32(uncached_pa); bench_puts("\n");
    bench_puts("Timer freq:           ");
    bench_print_uint(bench_timer_freq_hz()); bench_puts(" Hz\n\n");

    bench_puts("-- Cache hit (512 B working set, fits in 1 KiB D-cache) --\n");
    run_sweep_test("hit LDW", sweep_ldw, cached_base, HIT_WORKING_BYTES, 4);
    run_sweep_test("hit STW", sweep_stw, cached_base, HIT_WORKING_BYTES, 4);

    bench_puts("\n-- Cached sweep (64 KiB, sequential) --\n");
    run_sweep_test("cached LDW", sweep_ldw, cached_base, CACHED_WORKING_BYTES, 4);
    run_sweep_test("cached LDH", sweep_ldh, cached_base, CACHED_WORKING_BYTES, 2);
    run_sweep_test("cached LDB", sweep_ldb, cached_base, CACHED_WORKING_BYTES, 1);
    run_sweep_test("cached STW", sweep_stw, cached_base, CACHED_WORKING_BYTES, 4);
    run_sweep_test("cached STH", sweep_sth, cached_base, CACHED_WORKING_BYTES, 2);
    run_sweep_test("cached STB", sweep_stb, cached_base, CACHED_WORKING_BYTES, 1);

    bench_puts("\n-- Cache miss (64 KiB, stride 64 B, 100% line-fill misses) --\n");
    run_miss_test("miss LDW", sweep_miss_ldw, cached_base,
                  CACHED_WORKING_BYTES, MISS_STRIDE_BYTES);

    bench_puts("\n-- Uncached single-page latency (4 KiB, pinned NC) --\n");
    run_uncached_ldw(UNCACHED_TEST_VA);
    run_uncached_ldh(UNCACHED_TEST_VA);
    run_uncached_ldb(UNCACHED_TEST_VA);
    run_uncached_stw(UNCACHED_TEST_VA);
    run_uncached_sth(UNCACHED_TEST_VA);
    run_uncached_stb(UNCACHED_TEST_VA);

    bench_puts("\n=== membench done ===\n");
}

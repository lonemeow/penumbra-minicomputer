/*
 * entry.c — bench_main for the memory hierarchy bench.
 *
 * Measures the path from the CPU to memory, at three levels:
 *
 *   • Cache hit — a working set inside the L1, the upper bound on
 *     throughput when nothing reaches memory at all.
 *   • Sub-word sweep — half-word and byte accesses across the full
 *     working set, which exercise extract/replicate on the way.
 *   • Memory path — hand-written loops (membench/loops.S) over a
 *     working set several times the outermost cache, reported in
 *     cycles per L2 read miss rather than wall-clock throughput.
 *   • Uncached page — a pinned NC page, where every access is a bare
 *     memory round trip with no cache in the way.
 *
 * Working sets are derived at run time from each cache's INFO
 * register.  Sizing them by constant is how a memory benchmark stops
 * measuring memory: a figure chosen to outrun a 1 KiB L1 sits entirely
 * inside a 64 KiB L2, and the tests then report the cache they meant
 * to defeat.
 *
 * The memory-path tests report cycles per miss because that ratio is
 * indifferent to code quality — the stall counters are mutually
 * exclusive, so stall_load holds only cycles a read spent waiting, and
 * the miss count comes from the cache.  Wall-clock throughput, which
 * the other tests report, does move with how well the loop is
 * scheduled; that is the right number for them and the wrong one here.
 *
 * Each test runs for ~MEAS_TARGET_US of measured time.
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

/* ── Test geometry ────────────────────────────────────────────── */
/* Working sets are derived from the cache geometry the machine reports,
 * not written down here.  A constant chosen against one hierarchy
 * quietly becomes a cache test on the next: 64 KiB outran a 1 KiB L1 by
 * 64x, and fits entirely inside a 64 KiB L2.
 *
 * MEMORY_WORKING_MULTIPLE sets how far past the outermost cache a
 * memory test reaches, so each pass evicts what the last one left.
 * LOOP_GRANULE_BYTES is the coarsest unroll in loops.S (the strided
 * loop's 8 x 64 B), which every working set must be a multiple of.
 * HIT_WORKING_FRACTION keeps the hit test inside the L1 with room to
 * spare for the loop's own footprint. */
#define MEMORY_WORKING_MULTIPLE 4u
#define MEMORY_WORKING_FALLBACK (256u * 1024u)  /* no cache reports geometry */
#define LOOP_GRANULE_BYTES      512u
#define HIT_WORKING_FRACTION    2u              /* half the L1 */
#define HIT_WORKING_FALLBACK    (512u)
#define MISS_STRIDE_BYTES       64u             /* MEMBENCH_STRIDE in loops.S */
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

/* ── Hand-written access loops (membench/loops.S) ───────────────
 * Frozen instruction streams with an exact access count, so a number
 * recorded today still means the same thing after the compiler moves.
 * The "use" forms fold each result into the return value and measure
 * how fast the core consumes loaded data; the "discard" forms never
 * read what they loaded and measure the memory system's delivery rate.
 * They coincide on a core that stalls until load data returns. */
uint32_t membench_seq_ldw_use(uint32_t base, uint32_t bytes);
uint32_t membench_seq_ldw_discard(uint32_t base, uint32_t bytes);
uint32_t membench_stride_ldw_use(uint32_t base, uint32_t bytes);
uint32_t membench_stride_ldw_discard(uint32_t base, uint32_t bytes);

static uint32_t round_up_pow2(uint32_t v, uint32_t granule) {
    return (v + granule - 1u) & ~(granule - 1u);
}

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


/* ── Run one memory-hierarchy test ─────────────────────────────
 * Wraps a pass loop in the CPU stall counters and the L2 cache
 * counters, and reports cycles per L2 read miss.
 *
 * That ratio is the number to compare across memory-system changes,
 * because neither term moves with code quality: the counters are
 * mutually exclusive, so stall_load holds only cycles a data read
 * spent waiting, and the miss count comes from the cache itself.  A
 * worse-scheduled loop takes longer without changing either.
 *
 * The predicted miss count is printed beside the observed one.  The
 * loops are hand-written, so how many distinct lines a pass touches is
 * known exactly; a disagreement is a fact about the hardware rather
 * than a question about the code.
 */
static void run_memory_test(const char *label,
                            sweep_fn fn,
                            uint32_t base,
                            uint32_t working,
                            uint32_t access_stride,
                            uint32_t line_bytes) {
    bench_perf_t p0, p1;
    bench_cache_perf_t c0, c1;
    uint32_t deadline = bench_us_to_ticks(MEAS_TARGET_US);
    uint32_t sink = 0;
    uint32_t passes = 0;

    bench_perf_snapshot(&p0);
    bench_cache_perf_snapshot_l2(&c0);
    bench_timer_start();
    while (bench_timer_elapsed_ticks() < deadline) {
        sink ^= fn(base, working);
        passes++;
    }
    bench_cache_perf_snapshot_l2(&c1);
    bench_perf_snapshot(&p1);
    bench_sink ^= sink;

    /* A stride at least a line wide touches one line per access;
     * a narrower one touches every line in the working set. */
    uint32_t lines_per_pass = (access_stride >= line_bytes)
                            ? (working / access_stride)
                            : (working / line_bytes);
    uint32_t accesses  = passes * (working / access_stride);
    uint32_t predicted = passes * lines_per_pass;
    uint32_t observed  = c1.read_misses - c0.read_misses;
    uint32_t stall     = p1.stall_load  - p0.stall_load;

    bench_puts(label);
    bench_puts(": ");
    bench_print_uint(accesses);
    bench_puts(" reads, L2 read misses ");
    bench_print_uint(observed);
    bench_puts(" (predicted ");
    bench_print_uint(predicted);
    bench_puts(")\n  stall_load ");
    bench_print_uint(stall);
    bench_puts(" cyc");
    if (observed) {
        bench_puts(", ");
        bench_print_uint(stall / observed);
        bench_puts(" cyc/miss");
    }
    bench_puts("\n");
    bench_perf_print_delta(label, &p0, &p1);
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

    /* Size the working sets from what the machine says it has, so the
     * memory tests reach memory on any hierarchy rather than on the one
     * this file was written against. */
    bench_cache_geom_t l1d, outer;
    bench_cache_geom_l1d(&l1d);
    bench_cache_geom_outermost(&outer);

    uint32_t mem_working = outer.present
        ? round_up_pow2(outer.bytes * MEMORY_WORKING_MULTIPLE, LOOP_GRANULE_BYTES)
        : MEMORY_WORKING_FALLBACK;
    uint32_t hit_working = l1d.present
        ? round_up_pow2(l1d.bytes / HIT_WORKING_FRACTION, LOOP_GRANULE_BYTES)
        : HIT_WORKING_FALLBACK;
    uint32_t line_bytes  = outer.present ? outer.line_words * 4u : 16u;

    /* Layout in the test region:
     *   [reserved low ........................ MEMTEST_RESERVED_LO ]
     *   [working set, sized above                                  ]
     *   [uncached test page (PA), 4 KiB                            ]
     */
    uint32_t cached_base = (ram->base + MEMTEST_RESERVED_LO + 0xFFF) & ~0xFFFu;
    uint32_t uncached_pa = cached_base + mem_working;

    /* Sanity check: the test region must fit in the discovered RAM. */
    if (uncached_pa + UNCACHED_PAGE_BYTES > ram->base + ram->dev_size) {
        bench_puts("Insufficient RAM for working set — abort.\n");
        return;
    }

    /* The program's first pinned slot: the uncached test page */
    bench_tlb_pin(BENCH_PIN_FIRST_FREE, UNCACHED_TEST_VA,
                  (uncached_pa & ~0xFFFu) | PTE_KERNEL_NC);

    bench_puts("L1-D:                 ");
    if (l1d.present) {
        bench_print_uint(l1d.bytes); bench_puts(" B, ");
        bench_print_uint(l1d.line_words * 4u); bench_puts(" B lines, ");
        bench_print_uint(l1d.num_ways); bench_puts("-way\n");
    } else bench_puts("absent\n");
    bench_puts("Outermost cache:      ");
    if (outer.present) {
        bench_print_uint(outer.bytes); bench_puts(" B, ");
        bench_print_uint(line_bytes); bench_puts(" B lines, ");
        bench_print_uint(outer.num_ways); bench_puts("-way\n");
    } else bench_puts("absent\n");
    bench_puts("Working set:          ");
    bench_print_uint(mem_working); bench_puts(" B at ");
    print_hex32(cached_base); bench_puts("\n");
    bench_puts("Uncached page:        VA ");
    print_hex32(UNCACHED_TEST_VA); bench_puts(" -> PA ");
    print_hex32(uncached_pa); bench_puts("\n");
    bench_puts("Timer freq:           ");
    bench_print_uint(bench_timer_freq_hz()); bench_puts(" Hz\n\n");

    bench_puts("-- Cache hit (fits in L1-D) --\n");
    run_sweep_test("hit LDW", sweep_ldw, cached_base, hit_working, 4);
    run_sweep_test("hit STW", sweep_stw, cached_base, hit_working, 4);

    bench_puts("\n-- Sub-word sweep over the working set --\n");
    run_sweep_test("sweep LDH", sweep_ldh, cached_base, mem_working, 2);
    run_sweep_test("sweep LDB", sweep_ldb, cached_base, mem_working, 1);
    run_sweep_test("sweep STW", sweep_stw, cached_base, mem_working, 4);
    run_sweep_test("sweep STH", sweep_sth, cached_base, mem_working, 2);
    run_sweep_test("sweep STB", sweep_stb, cached_base, mem_working, 1);

    /* The memory path proper: hand-written loops, counter-measured.
     * Sequential is what a line fill looks like — a memory answering
     * with more than one word serves the rest without another round
     * trip.  Strided touches one word per line, so it gets nothing
     * from that and is the check that the same change costs nothing
     * where it cannot help. */
    bench_puts("\n-- Memory path, sequential --\n");
    run_memory_test("seq load-use    ", membench_seq_ldw_use,
                    cached_base, mem_working, 4u, line_bytes);
    run_memory_test("seq load-discard", membench_seq_ldw_discard,
                    cached_base, mem_working, 4u, line_bytes);

    bench_puts("\n-- Memory path, one word per line --\n");
    run_memory_test("stride load-use    ", membench_stride_ldw_use,
                    cached_base, mem_working, MISS_STRIDE_BYTES, line_bytes);
    run_memory_test("stride load-discard", membench_stride_ldw_discard,
                    cached_base, mem_working, MISS_STRIDE_BYTES, line_bytes);

    bench_puts("\n-- Uncached single-page latency (4 KiB, pinned NC) --\n");
    run_uncached_ldw(UNCACHED_TEST_VA);
    run_uncached_ldh(UNCACHED_TEST_VA);
    run_uncached_ldb(UNCACHED_TEST_VA);
    run_uncached_stw(UNCACHED_TEST_VA);
    run_uncached_sth(UNCACHED_TEST_VA);
    run_uncached_stb(UNCACHED_TEST_VA);

    bench_puts("\n=== membench done ===\n");
}

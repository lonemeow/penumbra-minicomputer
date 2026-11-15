/*
 * entry.c — bench_main for memspeed, a sweep of transfer rate against
 * block size.
 *
 * For each block size, from well inside the L1 to well past the
 * outermost cache, memspeed reads the block, writes it and copies it
 * to a second block, over and over, and reports the rate each
 * achieved.  Read in order, the rows show each level of the hierarchy
 * as a plateau and the steps between them where a block outgrows a
 * cache: the whole path a program's data takes, the miss handler's TLB
 * refills included once a block outgrows the TLB's reach.
 *
 * The transfers are the hand-written loops of memspeed/loops.S, so a
 * rate belongs to the memory system rather than to the compiler.  The
 * harness only places the blocks, times the loops and prints.
 *
 * Each measurement is one call that repeats its pass over the block a
 * number of times, found by doubling until one call lasts
 * MEAS_TARGET_US.  Every probe is a single timed call, the same shape
 * as the measurement, so no per-call overhead goes uncounted; the
 * probe that reaches the target is the measurement, run warm because
 * a warm-up pass and the probes before it brought the block in.
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

/* ── The loops (memspeed/loops.S) ─────────────────────────────── */
void memspeed_read(uint32_t base, uint32_t bytes, uint32_t passes);
void memspeed_write(uint32_t base, uint32_t bytes, uint32_t passes,
                    uint32_t value);
void memspeed_copy(uint32_t dst, uint32_t src, uint32_t bytes,
                   uint32_t passes);

/* ── Sweep geometry ───────────────────────────────────────────── */
/* BLOCK_GRANULE is the bytes one loop iteration moves (ITER_BYTES in
 * loops.S), which every block is a multiple of.  The smallest block
 * runs a few iterations a pass so the pass's own control stays a small
 * share of it.  The sweep reaches REACH_MULTIPLE times the outermost
 * cache, far enough past it that the last rows show memory alone. */
#define BLOCK_GRANULE        128u
#define MIN_BLOCK            (4u * BLOCK_GRANULE)
#define REACH_MULTIPLE       64u
#define REACH_FALLBACK       (4u * 1024u * 1024u)  /* no cache reports geometry */
#define PROGRAM_RESERVED     (2u * 1024u * 1024u)  /* the image, stack and harness */
#define PAGE_BYTES           4096u

/* Each measurement lasts at least this long: long enough that timer
 * granularity and the timer's own interrupt are noise, short enough
 * that the sweep's ~80 measurements finish in well under a minute. */
#define MEAS_TARGET_US       50000u

#define WRITE_VALUE          0x5AA5C33Cu
#define GUARD_VALUE          0xDEADBEEFu

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

/* ── Fixed-width output ───────────────────────────────────────── */
static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void print_padded(const char *s, uint32_t len, uint32_t width) {
    for (uint32_t i = len; i < width; i++)
        bench_putchar(' ');
    for (uint32_t i = 0; i < len; i++)
        bench_putchar(s[i]);
}

/* Append v's decimal digits to buf at *len, at least `min_digits`. */
static void append_uint(char *buf, uint32_t *len, uint32_t v,
                        uint32_t min_digits) {
    char rev[10];
    uint32_t n = 0;
    do {
        rev[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n < min_digits)
        rev[n++] = '0';
    while (n)
        buf[(*len)++] = rev[--n];
}

static void append_str(char *buf, uint32_t *len, const char *s) {
    while (*s)
        buf[(*len)++] = *s++;
}

/* Print a value held in units of 1/10^decimals, right-aligned. */
static void print_fixed(uint64_t scaled, uint32_t decimals, uint32_t width) {
    uint32_t div = 1;
    for (uint32_t i = 0; i < decimals; i++)
        div *= 10u;
    char buf[24];
    uint32_t len = 0;
    append_uint(buf, &len, (uint32_t)(scaled / div), 1);
    if (decimals) {
        buf[len++] = '.';
        append_uint(buf, &len, (uint32_t)(scaled % div), decimals);
    }
    print_padded(buf, len, width);
}

/* "512 B", "1.5 KiB", "96 KiB", "4 MiB": every block is a whole or a
 * half multiple of its unit, which is how the sweep steps. */
static void print_size(uint32_t bytes, uint32_t width) {
    static const char *const units[] = { " B", " KiB", " MiB" };
    uint32_t u = 0, whole = bytes, frac = 0;
    while (u < 2 && whole >= 1024u) {
        frac  = whole % 1024u;
        whole = whole / 1024u;
        u++;
    }
    char buf[16];
    uint32_t len = 0;
    append_uint(buf, &len, whole, 1);
    if (frac)
        append_str(buf, &len, ".5");
    append_str(buf, &len, units[u]);
    print_padded(buf, len, width);
}

static void print_hex32(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    bench_puts("0x");
    for (int sh = 28; sh >= 0; sh -= 4)
        bench_putchar(digits[(v >> sh) & 0xFu]);
}

static void print_geom(const char *label, const bench_cache_geom_t *g) {
    bench_puts(label);
    if (!g->present) {
        bench_puts("absent\n");
        return;
    }
    print_size(g->bytes, 0);
    bench_puts(", ");
    bench_print_uint(g->num_ways);
    bench_puts("-way, ");
    bench_print_uint(g->line_words * 4u);
    bench_puts(" B lines\n");
}

/* ── Measurement ──────────────────────────────────────────────── */
enum op { OP_READ, OP_WRITE, OP_COPY, OP_COUNT };

static uint32_t src_base, dst_base;

static void run_op(enum op op, uint32_t bytes, uint32_t passes) {
    switch (op) {
    case OP_READ:  memspeed_read(src_base, bytes, passes);              break;
    case OP_WRITE: memspeed_write(dst_base, bytes, passes, WRITE_VALUE); break;
    case OP_COPY:  memspeed_copy(dst_base, src_base, bytes, passes);    break;
    default:       break;
    }
}

typedef struct {
    uint64_t bytes;     /* bytes the measured call moved, counted once */
    uint32_t ticks;
    uint32_t cycles;
} result_t;

static void measure(enum op op, uint32_t bytes, uint32_t target_ticks,
                    result_t *r) {
    run_op(op, bytes, 1);
    for (uint32_t passes = 1;; passes *= 2) {
        bench_perf_t p0, p1;
        bench_perf_snapshot(&p0);
        bench_timer_start();
        run_op(op, bytes, passes);
        uint32_t ticks = bench_timer_elapsed_ticks();
        bench_perf_snapshot(&p1);
        if (ticks >= target_ticks) {
            r->bytes  = (uint64_t)passes * bytes;
            r->ticks  = ticks;
            r->cycles = p1.cycles - p0.cycles;
            return;
        }
    }
}

/* MB/s (10^6 bytes) to one decimal, and CPU cycles per word moved to
 * two.  A copy moves each word once in these terms, though memory
 * sees it twice, read and written. */
static void print_result(const result_t *r, uint32_t timer_hz) {
    uint64_t tenths = r->bytes * timer_hz / ((uint64_t)r->ticks * 100000u);
    uint64_t words  = r->bytes / 4u;
    uint64_t centi  = (uint64_t)r->cycles * 100u / words;
    print_fixed(tenths, 1, 10);
    print_fixed(centi, 2, 7);
}

/* The innermost cache that holds `footprint` bytes, or memory. */
static const char *level_for(uint32_t footprint,
                             const bench_cache_geom_t *l1d,
                             const bench_cache_geom_t *l2) {
    if (l1d->present && footprint <= l1d->bytes)
        return "L1";
    if (l2->present && footprint <= l2->bytes)
        return "L2";
    return "mem";
}

/* ── Loop self-check ──────────────────────────────────────────────
 * A loop that moves the wrong bytes reports a rate for the wrong
 * work, so each is checked once against its contract before anything
 * is timed: every word of the block written, the word past its end
 * left alone, and two passes ending where one would.  A read has no
 * result to check; it has to return, with the registers it borrowed
 * restored, or nothing after it works. */
static uint32_t pattern(uint32_t i) {
    return i * 0x9E3779B9u + 0x12345u;
}

static int check_block(uint32_t bytes) {
    volatile uint32_t *s = (volatile uint32_t *)src_base;
    volatile uint32_t *d = (volatile uint32_t *)dst_base;
    uint32_t n = bytes / 4u;

    for (uint32_t i = 0; i < n; i++) {
        s[i] = pattern(i);
        d[i] = ~pattern(i);
    }
    s[n] = GUARD_VALUE;
    d[n] = GUARD_VALUE;

    memspeed_read(src_base, bytes, 2);

    memspeed_copy(dst_base, src_base, bytes, 2);
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != pattern(i))
            return 0;
    if (d[n] != GUARD_VALUE || s[n] != GUARD_VALUE)
        return 0;

    memspeed_write(dst_base, bytes, 2, WRITE_VALUE);
    for (uint32_t i = 0; i < n; i++)
        if (d[i] != WRITE_VALUE)
            return 0;
    if (d[n] != GUARD_VALUE)
        return 0;

    for (uint32_t i = 0; i < n; i++)
        if (s[i] != pattern(i))
            return 0;
    return 1;
}

/* ── bench_main ───────────────────────────────────────────────── */
void bench_main(uint32_t bootdata) {
    bench_init();

    bench_puts("\n=== Penumbra memspeed ===\n");

    const struct btag_device *ram = find_first_memory_region(bootdata);
    if (!ram) {
        bench_puts("No ACFG_CLASS_MEMORY entry — abort.\n");
        return;
    }

    bench_cache_geom_t l1d, l2, outer;
    bench_cache_geom_l1d(&l1d);
    bench_cache_geom_l2(&l2);
    bench_cache_geom_outermost(&outer);

    uint32_t tlb_sets, tlb_ways;
    bench_tlb_geom(&tlb_sets, &tlb_ways);

    /* The copy's destination starts far enough past the source that
     * line i of the two blocks falls in different sets of every cache,
     * and page i in different sets of the TLB.  The plain end of the
     * largest block is a multiple of every way, which would put them in
     * the same set everywhere; in a two-way TLB the two blocks' pages
     * would then fill whole sets and evict the loop's own code page on
     * every pass, a cost of the layout rather than of the memory.
     *
     * Half the TLB's span of sets moves page i half way round them, so
     * blocks of up to that many pages never share a set.  Half an L1-D
     * way does the same for lines, in every cache whose way is larger
     * than it. */
    uint32_t tlb_span  = tlb_sets * PAGE_BYTES;
    uint32_t line_skew = BLOCK_GRANULE;
    if (l1d.present && l1d.bytes / l1d.num_ways / 2u > line_skew)
        line_skew = l1d.bytes / l1d.num_ways / 2u;

    uint32_t ram_end = ram->base + ram->dev_size;
    src_base = (ram->base + PROGRAM_RESERVED + PAGE_BYTES - 1u)
             & ~(PAGE_BYTES - 1u);
    if (src_base >= ram_end) {
        bench_puts("Insufficient RAM — abort.\n");
        return;
    }
    uint32_t avail = ram_end - src_base;

    /* The source, the distance to the destination, the destination,
     * and a guard word past it. */
    uint32_t max_block = outer.present ? outer.bytes * REACH_MULTIPLE
                                       : REACH_FALLBACK;
    uint32_t gap;
    for (;;) {
        gap = (max_block + tlb_span - 1u) / tlb_span * tlb_span
            + tlb_span / 2u + line_skew;
        if (max_block < MIN_BLOCK || (uint64_t)gap + max_block + 4u <= avail)
            break;
        max_block /= 2u;
    }
    if (max_block < MIN_BLOCK || max_block % BLOCK_GRANULE) {
        bench_puts("Insufficient RAM for the sweep — abort.\n");
        return;
    }
    dst_base = src_base + gap;

    char name[BENCH_NAME_LEN];
    bench_machine_name(name);
    bench_puts("Machine:     ");
    bench_puts(name);
    bench_cpu_name(name);
    bench_puts("\nCPU:         ");
    bench_puts(name);
    uint32_t cpu_hz = bench_cpu_freq_hz();
    if (cpu_hz) {
        bench_puts(" at ");
        bench_print_milli((uint64_t)cpu_hz / 1000u);
        bench_puts(" MHz");
    }
    const bench_memory_t *mem = bench_machine_memory();
    bench_puts("\nMemory:      ");
    if (!mem)
        bench_puts("not described for this machine");
    else if (mem->simulated)
        bench_puts("simulated, untimed");
    else
        bench_puts(mem->memory);
    bench_puts("\n");
    print_geom("L1-D:        ", &l1d);
    print_geom("L2:          ", &l2);
    bench_puts("TLB:         ");
    bench_print_uint(tlb_sets);
    bench_puts(" sets, ");
    bench_print_uint(tlb_ways);
    bench_puts("-way, reach ");
    print_size(tlb_sets * tlb_ways * PAGE_BYTES, 0);
    bench_puts("\n");
    bench_puts("Blocks:      ");
    print_size(MIN_BLOCK, 0);
    bench_puts(" to ");
    print_size(max_block, 0);
    bench_puts(", source at ");
    print_hex32(src_base);
    bench_puts(", copy destination at ");
    print_hex32(dst_base);
    bench_puts("\n");
    if (mem && mem->simulated)
        bench_puts("*** Simulated machine: the rates below count "
                   "instructions, not time.\n");

    if (!check_block(MIN_BLOCK) || !check_block(3u * MIN_BLOCK)) {
        bench_puts("*** A transfer loop broke its contract — abort.\n");
        return;
    }

    bench_puts("\nRates in MB/s (10^6 bytes) and CPU cycles per 32-bit word.\n"
               "Copy counts each word once; memory reads it and writes it.\n"
               "Level: the innermost cache a block fits in.\n\n");
    bench_puts("     Block Level  |      Read  cyc/w |     Write  cyc/w "
               "|      Copy  cyc/w\n");
    bench_puts("     ----- -----  |   -------  ----- |   -------  ----- "
               "|   -------  -----\n");

    uint32_t timer_hz     = bench_timer_freq_hz();
    uint32_t target_ticks = bench_us_to_ticks(MEAS_TARGET_US);

    for (uint32_t octave = MIN_BLOCK; octave <= max_block; octave *= 2u) {
        /* Two steps an octave: the power of two, and half again. */
        for (uint32_t step = 0; step < 2; step++) {
            uint32_t bytes = step ? octave + octave / 2u : octave;
            if (bytes > max_block)
                break;

            const char *level = level_for(bytes, &l1d, &l2);
            print_size(bytes, 10);
            print_padded(level, str_len(level), 6);
            bench_puts("  |");

            for (enum op op = OP_READ; op < OP_COUNT; op++) {
                result_t r;
                measure(op, bytes, target_ticks, &r);
                print_result(&r, timer_hz);
                bench_puts(op + 1 < OP_COUNT ? " |" : "\n");
            }
        }
    }

    bench_puts("\n=== memspeed done ===\n");
}

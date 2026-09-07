/*
 * bandwidth.c — access-cost characterization for the display aperture.
 *
 * The framebuffer lives in the display device's own BRAM, reached over
 * the system bus rather than through memory, and the bus expresses one
 * master — so every visible pixel is a CPU store, and what that store
 * costs is the ceiling on any full-screen effect.  This measures it at
 * each access width the compiler can emit, under both cache mappings,
 * with SDRAM beside it so rendering into a backbuffer and blitting can
 * be priced against drawing straight into the aperture.
 *
 * The aperture is uncached by policy rather than by hardware: the
 * bare-metal TLB miss handler maps addresses at or above 0xFF000000
 * without PTE.C, and devices live above that line.  A cached mapping
 * therefore needs a pinned TLB entry, which wins over the main TLB.
 * Caching a device aperture is sound here for a reason peculiar to
 * this device: scan-out only ever reads the pixel array, so the CPU is
 * its sole writer and a cached copy cannot go stale.  That does not
 * generalise to a device with status registers.
 *
 * Numbers only mean something on hardware.  The ISS models no caches
 * and no bus timing and reports the same figure for every row; the
 * suite still runs there, as a check that the TLB manipulation is
 * well-formed and that nothing faults.
 */

#include "bandwidth.h"
#include "display.h"
#include "penumbra.h"

/* ── MMU sysregs ───────────────────────────────────────────────
 *
 * penumbra.h carries the cache and CPU register maps but not the TLB
 * ones.  Layout and semantics are in doc/system/mmu.md.
 */
#define SYSDEV_MMU      0
#define MMU_TLB_VPN     3
#define MMU_TLB_PTE     4
#define MMU_TLB_INDEX   5
#define PTE_C           0x04    /* cacheable */
#define TLB_SETS        32
#define TLB_WAYS        2

/* The pixel array at the geometry this device declares.  Sizes the
 * SDRAM comparison buffer, and bounds a sweep if a future device
 * declares more than this runtime is built to hold. */
#define FB_MAX_BYTES    76800u
#define FB_MAX_WORDS    (FB_MAX_BYTES / 4)

/* A sweep small enough to sit inside the L2 against the full frame,
 * which at 320x240 is 76800 bytes and does not.  256x192 is the
 * viewport that buys exactly this, so the pair prices that choice
 * rather than leaving it to be argued from geometry. */
#define L2_FIT_WORDS    (49152u / 4)

/* Repetition counts chosen so every row totals roughly 150k accesses:
 * long enough that counter granularity and loop entry cost vanish,
 * short enough that a row stays well inside the 32-bit cycle counter. */
#define FRAME_REPS      8
#define BYTE_REPS       2
#define L2_FIT_REPS     12

static uint32_t disp_base;
static uint32_t fb_words;       /* whole framebuffer, in 32-bit words */
static uint32_t fb_stride;      /* bytes per pixel row */
static uint32_t fb_rows;

/* Keeps measured loads from being dead code. */
static volatile uint32_t sink;

/* A cached-memory buffer the size of a whole frame, so the SDRAM rows
 * and the aperture rows differ only in what they address. */
static uint32_t sdram_window[FB_MAX_WORDS];

/* ── Reporting ────────────────────────────────────────────────── */

static void puts_pad(const char *s, int width)
{
    int n = 0;
    while (s[n]) {
        bench_putchar(s[n]);
        n++;
    }
    while (n++ < width)
        bench_putchar(' ');
}

/* Two decimals without a divide-heavy formatter: the fraction is taken
 * from the remainder, which is smaller than the divisor and so cannot
 * overflow when scaled by 100. */
static void print_hundredths(uint32_t whole, uint32_t frac)
{
    bench_print_uint(whole);
    bench_putchar('.');
    bench_putchar((char)('0' + (frac / 10) % 10));
    bench_putchar((char)('0' + frac % 10));
}

static void report(const char *label, uint32_t ops,
                   const bench_perf_t *a, const bench_perf_t *b)
{
    uint32_t cycles = b->cycles - a->cycles;
    uint32_t stalls = (b->stall_store - a->stall_store)
                    + (b->stall_load  - a->stall_load);

    bench_puts("  ");
    puts_pad(label, 28);

    if (ops == 0) {
        bench_puts("no accesses\r\n");
        return;
    }

    bench_print_uint(cycles);
    bench_puts(" cyc  ");
    print_hundredths(cycles / ops, ((cycles % ops) * 100u) / ops);
    bench_puts(" cyc/op");

    if (stalls) {
        bench_puts("   stall ");
        bench_print_uint(cycles ? stalls * 100u / cycles : 0);
        bench_puts("%");
    }
    bench_puts("\r\n");
}

/* ── Access patterns ──────────────────────────────────────────── */
/*
 * Every loop is unrolled eight deep, so loop overhead is one branch and
 * one add per eight accesses rather than per access, and so the store
 * rows match the shape the drawing primitives already use.
 */

static void store_words(volatile uint32_t *p, uint32_t words, uint32_t val)
{
    while (words >= 8) {
        p[0] = val; p[1] = val; p[2] = val; p[3] = val;
        p[4] = val; p[5] = val; p[6] = val; p[7] = val;
        p += 8;
        words -= 8;
    }
    while (words--)
        *p++ = val;
}

static void store_bytes(volatile unsigned char *p, uint32_t bytes,
                        unsigned char val)
{
    while (bytes >= 8) {
        p[0] = val; p[1] = val; p[2] = val; p[3] = val;
        p[4] = val; p[5] = val; p[6] = val; p[7] = val;
        p += 8;
        bytes -= 8;
    }
    while (bytes--)
        *p++ = val;
}

/* One word per pixel row: the address pattern a vertical span produces
 * before it is batched into four-column groups.  Nothing to unroll —
 * the stride is the point. */
static void store_words_strided(volatile unsigned char *p, uint32_t count,
                                uint32_t stride, uint32_t val)
{
    while (count >= 8) {
        volatile unsigned char *q = p;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        *(volatile uint32_t *)q = val; q += stride;
        p = q;
        count -= 8;
    }
    while (count--) {
        *(volatile uint32_t *)p = val;
        p += stride;
    }
}

static void load_words(const volatile uint32_t *p, uint32_t words)
{
    uint32_t acc = 0;

    while (words >= 8) {
        acc += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
        p += 8;
        words -= 8;
    }
    while (words--)
        acc += *p++;

    sink = acc;
}

/* Read-modify-write: the shape of any effect that needs last frame's
 * pixels — fire, blur, motion trails, sprite restore. */
static void rmw_words(volatile uint32_t *p, uint32_t words)
{
    while (words--) {
        *p = *p + 0x01010101u;
        p++;
    }
}

/* The same loop skeleton with no memory access, so a row can be read
 * net of the loop driving it.  The barrier is what keeps the loop from
 * collapsing into arithmetic — the measured loops are held together by
 * their volatile accesses, and this one has none. */
static void spin_words(uint32_t words)
{
    while (words >= 8) {
        __asm__ __volatile__("" : : : "memory");
        words -= 8;
    }
    while (words--)
        __asm__ __volatile__("" : : : "memory");
}

/* ── Cache control ────────────────────────────────────────────── */

static int cache_present(uint32_t info)
{
    return info != 0;
}

static void print_cache_geometry(const char *name, uint32_t info)
{
    bench_puts("  ");
    puts_pad(name, 10);
    if (!cache_present(info)) {
        bench_puts("absent\r\n");
        return;
    }
    bench_print_uint(CACHE_INFO_NUM_SETS(info));
    bench_puts(" sets x ");
    bench_print_uint(CACHE_INFO_NUM_WAYS(info));
    bench_puts(" ways x ");
    bench_print_uint(CACHE_INFO_LINE_WORDS(info));
    bench_puts(" words = ");
    bench_print_uint(CACHE_INFO_NUM_SETS(info) * CACHE_INFO_NUM_WAYS(info)
                     * CACHE_INFO_LINE_WORDS(info) * 4u);
    bench_puts(" bytes, ");
    bench_puts(CACHE_INFO_WRITE_BACK(info) ? "write-back" : "write-through");
    bench_puts(", ");
    bench_puts(CACHE_INFO_WRITE_ALLOC(info) ? "write-allocate"
                                            : "no-write-allocate");
    bench_puts("\r\n");
}

/* Drop every cached line so the next sweep is genuinely cold.  The
 * invalidate is multi-cycle on both levels, so each is polled to
 * completion before the measurement that depends on it starts. */
static void invalidate_caches(void)
{
    if (cache_present(penumbra_read_sysreg(SYSDEV_L1_DCACHE, CACHE_INFO))) {
        penumbra_write_sysreg(SYSDEV_L1_DCACHE, CACHE_INVAL_ALL, 0);
        while (penumbra_read_sysreg(SYSDEV_L1_DCACHE, CACHE_STATUS)
               & CACHE_STATUS_BUSY)
            ;
    }
    if (cache_present(penumbra_read_sysreg(SYSDEV_L2_CACHE, CACHE_INFO))) {
        penumbra_write_sysreg(SYSDEV_L2_CACHE, CACHE_INVAL_ALL, 0);
        while (penumbra_read_sysreg(SYSDEV_L2_CACHE, CACHE_STATUS)
               & CACHE_STATUS_BUSY)
            ;
    }
}

/* ── The suite ────────────────────────────────────────────────── */

static void measure_uncached(void)
{
    volatile uint32_t *fb = (volatile uint32_t *)(disp_base + PD_FB);
    volatile unsigned char *fbb = (volatile unsigned char *)(disp_base + PD_FB);
    bench_perf_t a, b;

    bench_puts("\r\n  uncached aperture (the mapping the miss handler"
               " installs)\r\n");

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        store_words(fb, fb_words, 0x01010101u);
    bench_perf_snapshot(&b);
    report("word store, sequential", fb_words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < BYTE_REPS; i++)
        store_bytes(fbb, fb_words * 4u, 0x02);
    bench_perf_snapshot(&b);
    report("byte store, sequential", fb_words * 4u * BYTE_REPS, &a, &b);

    /* One column walked repeatedly rather than every column once: the
     * access pattern is what is under test, and repeating one column
     * keeps the op count level with the sequential rows. */
    uint32_t strided_reps = (fb_words * FRAME_REPS) / fb_rows;
    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < strided_reps; i++)
        store_words_strided(fbb, fb_rows, fb_stride, 0x03030303u);
    bench_perf_snapshot(&b);
    report("word store, column walk", fb_rows * strided_reps, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        load_words(fb, fb_words);
    bench_perf_snapshot(&b);
    report("word load, sequential", fb_words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        rmw_words(fb, fb_words);
    bench_perf_snapshot(&b);
    report("word read-modify-write", fb_words * FRAME_REPS, &a, &b);
}

/* ── Cached mapping of the pixel aperture ─────────────────────── */
/*
 * What the miss handler decided for `va`, or 0 if the page is not
 * resident.  The set is VPN[4:0] and the handler alternates ways, so
 * both are searched for the matching tag.
 *
 * This is how the range is verified rather than the cache counters:
 * it needs only a working MMU, so a mistake in the handler's range
 * test shows up under simulation instead of costing a flash cycle.
 */
static uint32_t tlb_pte_for(uint32_t va)
{
    uint32_t vpn = va >> 12;
    uint32_t set = vpn & (TLB_SETS - 1);

    for (uint32_t way = 0; way < TLB_WAYS; way++) {
        penumbra_write_sysreg(SYSDEV_MMU, MMU_TLB_INDEX, set | (way << 5));
        if ((penumbra_read_sysreg(SYSDEV_MMU, MMU_TLB_VPN) >> 8) == vpn)
            return penumbra_read_sysreg(SYSDEV_MMU, MMU_TLB_PTE);
    }
    return 0;
}

/*
 * Caching a device aperture is sound here for a reason peculiar to
 * this device: scan-out only ever reads the pixel array, so the CPU is
 * its sole writer and a cached copy cannot go stale.  The range covers
 * the whole array rather than a sample of it, because the question the
 * rows answer is whether a full-screen sweep stays resident — and at
 * 76800 bytes against a 64 KiB L2 it cannot, which is exactly what a
 * smaller window would have hidden.
 */
static void measure_cached(uint32_t words)
{
    uint32_t fb = disp_base + PD_FB;
    volatile uint32_t *p = (volatile uint32_t *)fb;
    bench_perf_t a, b;
    bench_cache_perf_t ca, cb;

    bench_puts("\r\n  cached aperture (whole ");
    bench_print_uint(words * 4u);
    bench_puts("-byte pixel array)\r\n");

    bench_map_cached(fb, fb + words * 4u);
    invalidate_caches();

    /* Fault a page in, then read back the attribute the handler chose.
     * A range that never matches leaves the aperture uncached and every
     * row below silently measures the uncached path again. */
    sink = p[0];
    uint32_t pte = tlb_pte_for(fb);
    if (pte == 0)
        bench_puts("  !! no TLB entry for the aperture\r\n");
    else if (!(pte & PTE_C))
        bench_puts("  !! range not in effect — aperture PTE is uncached;"
                   " rows below are not what they say\r\n");
    else
        bench_puts("  range in effect — aperture PTE is cacheable\r\n");

    /* One cold sweep: every line fill paid once, nothing resident to
     * hit against, so this is the device's own fill cost. */
    bench_cache_perf_snapshot_l1d(&ca);
    bench_perf_snapshot(&a);
    load_words(p, words);
    bench_perf_snapshot(&b);
    bench_cache_perf_snapshot_l1d(&cb);
    report("word load, cold sweep", words, &a, &b);

    /* Corroborates the PTE readback where counters exist.  The ISS
     * reports none, so silence here is not itself a finding. */
    if ((cb.read_hits - ca.read_hits) == 0
        && (cb.read_misses - ca.read_misses) == 0) {
        bench_puts("  (no L1-D counter activity — expected on a"
                   " platform without cache counters)\r\n");
    }

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        load_words(p, words);
    bench_perf_snapshot(&b);
    report("word load, full frame", words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < L2_FIT_REPS; i++)
        load_words(p, L2_FIT_WORDS);
    bench_perf_snapshot(&b);
    report("word load, L2-fit sweep", L2_FIT_WORDS * L2_FIT_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        store_words(p, words, 0x04040404u);
    bench_perf_snapshot(&b);
    report("word store, full frame", words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        rmw_words(p, words);
    bench_perf_snapshot(&b);
    report("word RMW, full frame", words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < L2_FIT_REPS; i++)
        rmw_words(p, L2_FIT_WORDS);
    bench_perf_snapshot(&b);
    report("word RMW, L2-fit sweep", L2_FIT_WORDS * L2_FIT_REPS, &a, &b);

    /* Both levels over one full-frame sweep: the miss split is what
     * says whether the frame is resident or streaming from the device. */
    bench_cache_perf_snapshot_l1d(&ca);
    load_words(p, words);
    bench_cache_perf_snapshot_l1d(&cb);
    bench_cache_perf_print_delta("  L1-D, one full-frame sweep", &ca, &cb);

    bench_cache_perf_snapshot_l2(&ca);
    load_words(p, words);
    bench_cache_perf_snapshot_l2(&cb);
    bench_cache_perf_print_delta("  L2, one full-frame sweep", &ca, &cb);

    bench_map_cached(0, 0);
    invalidate_caches();
}

static void measure_sdram(uint32_t words)
{
    volatile uint32_t *p = (volatile uint32_t *)sdram_window;
    bench_perf_t a, b;

    bench_puts("\r\n  SDRAM, same size (what a backbuffer costs)\r\n");

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        store_words(p, words, 0x05050505u);
    bench_perf_snapshot(&b);
    report("word store, full frame", words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        load_words(p, words);
    bench_perf_snapshot(&b);
    report("word load, full frame", words * FRAME_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < L2_FIT_REPS; i++)
        load_words(p, L2_FIT_WORDS);
    bench_perf_snapshot(&b);
    report("word load, L2-fit sweep", L2_FIT_WORDS * L2_FIT_REPS, &a, &b);

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        rmw_words(p, words);
    bench_perf_snapshot(&b);
    report("word RMW, full frame", words * FRAME_REPS, &a, &b);
}

void fb_bandwidth_report(uint32_t base, uint32_t width, uint32_t height)
{
    bench_perf_t a, b;

    disp_base = base;
    fb_stride = width;
    fb_rows   = height;
    fb_words  = width * height / 4u;

    bench_puts("\r\n=== aperture access cost ===\r\n");

    print_cache_geometry("L1-D",
                         penumbra_read_sysreg(SYSDEV_L1_DCACHE, CACHE_INFO));
    print_cache_geometry("L2",
                         penumbra_read_sysreg(SYSDEV_L2_CACHE, CACHE_INFO));

    bench_perf_snapshot(&a);
    for (uint32_t i = 0; i < FRAME_REPS; i++)
        spin_words(fb_words);
    bench_perf_snapshot(&b);
    bench_puts("\r\n");
    report("loop overhead, no access", fb_words * FRAME_REPS, &a, &b);

    /* Both cached phases sweep the frame, and the SDRAM comparison
     * buffer has to match it, so a device declaring more than this
     * build reserves is measured over as much as fits. */
    uint32_t words = fb_words > FB_MAX_WORDS ? FB_MAX_WORDS : fb_words;

    measure_uncached();
    measure_cached(words);
    measure_sdram(words);

    bench_puts("\r\n  a full 320x240 frame is ");
    bench_print_uint(fb_words);
    bench_puts(" word stores; at 30 fps that is ");
    bench_print_uint(fb_words * 30u / 1000u);
    bench_puts("k stores/s\r\n");
}

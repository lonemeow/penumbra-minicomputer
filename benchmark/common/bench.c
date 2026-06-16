/*
 * bench.c — Penumbra bare-metal benchmark harness implementation
 *
 * Timer measurement uses the hardware timer (sysreg device 7) in
 * free-running autoload mode with TMRELOAD = 0xFFFE (period = 0xFFFF
 * ticks).  The IRQ handler (installed by crt0.S) accumulates raw
 * ticks at address 0x38 on each underflow (adds 0xFFFF per wrap).
 * The readback function adds the partial count.  Conversion to µs
 * uses the actual TMFREQ value via a 64-bit-precision (a*b)/d helper,
 * so the result is exact even when TMFREQ isn't an integer multiple
 * of 1 MHz (e.g. 1041666 Hz at 12.5 MHz, 961538 Hz at 25 MHz).
 */

#include "bench.h"

/* ── Stringification for inline asm sysreg operands ────────────── */
#define _STR(x)  #x
#define XSTR(x)  _STR(x)

/* ── UART registers (0xFF000000, word-strided, data in bits 7:0) ─ */
#define UART_BASE  0xFF000000
#define UART_THR   (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_LSR   (*(volatile uint32_t *)(UART_BASE + 0x14))
#define LSR_THRE   0x20

/* ── Timer base tick counter (on page 0, accumulated by IRQ handler) */
#define TIMER_BASE_ADDR   0x38
#define timer_base_ticks  (*(volatile uint32_t *)TIMER_BASE_ADDR)

/* Timer reload value: 0xFFFE gives period = 0xFFFF (65535) ticks,
 * which fits in a single ADD immediate in the ISR. */
#define TIMER_RELOAD      0xFFFE
#define TIMER_PERIOD      0xFFFF    /* reload + 1 */

/* Timer tick frequency in Hz, latched from TMFREQ at bench_init() */
static uint32_t timer_freq;

/* ── Timer sysregs (device 7) ──────────────────────────────────── */
#define SYSDEV_TIMER   7
#define TM_FREQ        0
#define TM_CR          1
#define TM_COUNT       2
#define TM_RELOAD      3
#define TM_STATUS      4
#define TMCR_TICK_EN   0x01
#define TMCR_IRQ_EN    0x02
#define TMCR_AUTOLOAD  0x04
#define TMST_UDF       0x01

/* ── CPU perfctr sysregs (device 1) ────────────────────────────── */
#define SYSDEV_CPU            1
#define CPU_CYCLES            5
#define CPU_INSNS_RETIRED     6
#define CPU_STALL_FUNIT       7
#define CPU_STALL_IFETCH      8
#define CPU_STALL_LOAD        9
#define CPU_STALL_STORE       10
#define CPU_STALL_HAZARD      11
#define CPU_STALL_FLUSH       12

/* ── Cache sysregs (devices 2, 3, 9 — identical layout) ────────── */
#define SYSDEV_L1_DCACHE      2
#define SYSDEV_L1_ICACHE      3
#define SYSDEV_L2_CACHE       9
#define CACHE_CTRL            1
#define CACHE_CTRL_ENABLE     1

/* Perfctr register numbers, identical across every cache device */
#define CACHE_READ_HITS       10
#define CACHE_READ_MISSES     11
#define CACHE_WRITE_HITS      12
#define CACHE_WRITE_MISSES    13

/* ── Sysreg access macros ──────────────────────────────────────── */
#define read_sysreg(dev, reg) ({                                    \
    uint32_t __v;                                                   \
    __asm__ __volatile__("rdsys %0, " XSTR(dev) ", " XSTR(reg)     \
                         : "=r"(__v));                              \
    __v;                                                            \
})

#define write_sysreg(dev, reg, val) do {                            \
    uint32_t __v = (val);                                           \
    __asm__ __volatile__("wrsys %0, " XSTR(dev) ", " XSTR(reg)     \
                         : : "r"(__v));                             \
} while (0)

static inline void disable_interrupts(void) {
    __asm__ __volatile__("di" ::: "memory");
}

static inline void enable_interrupts(void) {
    __asm__ __volatile__("ei" ::: "memory");
}

/* ── Timer ─────────────────────────────────────────────────────── */

void bench_init(void) {
    /* Read actual timer tick frequency from hardware */
    timer_freq = read_sysreg(SYSDEV_TIMER, TM_FREQ);
    if (timer_freq == 0)
        timer_freq = 1000000;       /* fallback if hardware misreports */

    /* Set up timer: free-running countdown, period = 0xFFFF ticks */
    timer_base_ticks = 0;
    write_sysreg(SYSDEV_TIMER, TM_RELOAD, TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_COUNT,  TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);   /* clear UDF */
    write_sysreg(SYSDEV_TIMER, TM_CR,
                 TMCR_TICK_EN | TMCR_IRQ_EN | TMCR_AUTOLOAD);
    enable_interrupts();
}

void bench_timer_start(void) {
    disable_interrupts();
    timer_base_ticks = 0;
    write_sysreg(SYSDEV_TIMER, TM_COUNT,  TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);   /* clear UDF */
    enable_interrupts();
}

/* Fast tick readback + µs conversion wrapper. */
uint32_t bench_timer_elapsed_ticks(void) {
    disable_interrupts();
    uint32_t count = read_sysreg(SYSDEV_TIMER, TM_COUNT);
    uint32_t base = timer_base_ticks;
    if (read_sysreg(SYSDEV_TIMER, TM_STATUS) & TMST_UDF) {
        /* Underflow occurred but ISR hasn't run yet — add one period
         * and re-read the count (timer may have already reloaded). */
        base += TIMER_PERIOD;
        count = read_sysreg(SYSDEV_TIMER, TM_COUNT);
    }
    enable_interrupts();
    return base + (TIMER_RELOAD - count);
}

/* Compute (a * b) / d at 64-bit precision using only 32-bit ops.
 * Caller guarantees the quotient fits in 32 bits and d > 0.
 * Forms the 64-bit product via four 16x16 multiplies, then long-divides. */
static uint32_t mul_div_u32(uint32_t a, uint32_t b, uint32_t d) {
    uint32_t a_hi = a >> 16, a_lo = a & 0xFFFF;
    uint32_t b_hi = b >> 16, b_lo = b & 0xFFFF;

    uint32_t hh = a_hi * b_hi;
    uint32_t hl = a_hi * b_lo;
    uint32_t lh = a_lo * b_hi;
    uint32_t ll = a_lo * b_lo;

    uint32_t mid     = (hl & 0xFFFF) + (lh & 0xFFFF) + (ll >> 16);
    uint32_t prod_lo = (mid << 16) | (ll & 0xFFFF);
    uint32_t prod_hi = hh + (hl >> 16) + (lh >> 16) + (mid >> 16);

    /* Long-divide [prod_hi:prod_lo] by d, MSB-first across 64 bits.
     * Top 32 bits feed the running remainder; only the low 32 quotient
     * bits are kept (caller asserts the quotient fits there). */
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((prod_hi >> i) & 1);
        if (r >= d) r -= d;
    }
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((prod_lo >> i) & 1);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}

uint32_t bench_us_to_ticks(uint32_t us) {
    return mul_div_u32(us, timer_freq, 1000000u);
}

uint32_t bench_timer_elapsed_us(void) {
    return mul_div_u32(bench_timer_elapsed_ticks(), 1000000u, timer_freq);
}

uint32_t bench_timer_freq_hz(void) {
    return timer_freq;
}

/* ── Cache control ─────────────────────────────────────────────── */

void bench_caches_enable(void) {
    write_sysreg(SYSDEV_L1_DCACHE, CACHE_CTRL, CACHE_CTRL_ENABLE);
    write_sysreg(SYSDEV_L1_ICACHE, CACHE_CTRL, CACHE_CTRL_ENABLE);
}

void bench_caches_disable(void) {
    write_sysreg(SYSDEV_L1_DCACHE, CACHE_CTRL, 0);
    write_sysreg(SYSDEV_L1_ICACHE, CACHE_CTRL, 0);
}

/* ── CPU performance counters ──────────────────────────────────── */

void bench_perf_snapshot(bench_perf_t *out) {
    out->cycles        = read_sysreg(SYSDEV_CPU, CPU_CYCLES);
    out->insns_retired = read_sysreg(SYSDEV_CPU, CPU_INSNS_RETIRED);
    out->stall_funit   = read_sysreg(SYSDEV_CPU, CPU_STALL_FUNIT);
    out->stall_ifetch  = read_sysreg(SYSDEV_CPU, CPU_STALL_IFETCH);
    out->stall_load    = read_sysreg(SYSDEV_CPU, CPU_STALL_LOAD);
    out->stall_store   = read_sysreg(SYSDEV_CPU, CPU_STALL_STORE);
    out->stall_hazard  = read_sysreg(SYSDEV_CPU, CPU_STALL_HAZARD);
    out->stall_flush   = read_sysreg(SYSDEV_CPU, CPU_STALL_FLUSH);
}

/* Print "<count> (<pct>.<frac>%)" — `count` and its share of `total`,
 * one fractional digit, overflow-safe; "(n/a)" when total is 0. */
static void print_count_pct(uint32_t count, uint32_t total) {
    bench_print_uint(count);
    bench_puts(" (");
    if (total == 0) { bench_puts("n/a)"); return; }
    /* (count * 1000) overflows if count >= 2^22 (~4.2M); scale both
     * down by the same shift to preserve the ratio. */
    uint32_t c = count, t = total;
    while (c > 0x003FFFFFu) { c >>= 1; t >>= 1; }
    uint32_t pct_x10 = (t > 0) ? (c * 1000) / t : 0;
    bench_print_uint(pct_x10 / 10);
    bench_putchar('.');
    bench_print_uint(pct_x10 % 10);
    bench_puts("%)");
}

void bench_perf_print_delta(const char *label,
                            const bench_perf_t *before,
                            const bench_perf_t *after) {
    /* 32-bit unsigned subtraction handles the wrap case correctly:
     * if `after.cycles` wrapped past `before.cycles`, the modular
     * subtraction still yields the true elapsed count. */
    uint32_t d_cycles = after->cycles        - before->cycles;
    uint32_t d_insns  = after->insns_retired - before->insns_retired;

    bench_puts(label);
    bench_puts(": ");
    bench_print_uint(d_cycles);
    bench_puts(" cycles, ");
    bench_print_uint(d_insns);
    bench_puts(" insns");

    if (d_insns > 0) {
        /* CPI = cycles / insns, displayed to 2 decimal places.
         * Stay in 32-bit: (cycles * 100) overflows if cycles >= 2^25
         * (~33M), so scale both operands down by the same shift first
         * to preserve the ratio.  Precision loss is bounded by the
         * shift count and is tiny relative to a 2-decimal display. */
        uint32_t c = d_cycles, n = d_insns;
        while (c > 0x01FFFFFFu) {     /* keep c * 100 < 2^32 */
            c >>= 1;
            n >>= 1;
        }
        uint32_t cpi_x100 = (n > 0) ? (c * 100) / n : 0;

        bench_puts(", CPI=");
        bench_print_uint(cpi_x100 / 100);
        bench_putchar('.');
        uint32_t frac = cpi_x100 % 100;
        if (frac < 10) bench_putchar('0');
        bench_print_uint(frac);
    }
    bench_puts("\n");

    /* Stall attribution: each bucket's cycles and its share of the
     * total.  Omitted when all four are zero, so output is unchanged
     * on the ISS (instruction-accurate, no stalls modeled). */
    uint32_t d_funit  = after->stall_funit  - before->stall_funit;
    uint32_t d_ifetch = after->stall_ifetch - before->stall_ifetch;
    uint32_t d_load   = after->stall_load   - before->stall_load;
    uint32_t d_store  = after->stall_store  - before->stall_store;
    uint32_t d_hazard = after->stall_hazard - before->stall_hazard;
    uint32_t d_flush  = after->stall_flush  - before->stall_flush;
    if (d_funit | d_ifetch | d_load | d_store | d_hazard | d_flush) {
        bench_puts("CPU stalls:   funit ");   /* aligns under CPU perfctrs: */
        print_count_pct(d_funit, d_cycles);
        bench_puts(", ifetch ");
        print_count_pct(d_ifetch, d_cycles);
        bench_puts(", load ");
        print_count_pct(d_load, d_cycles);
        bench_puts(", store ");
        print_count_pct(d_store, d_cycles);
        bench_puts("\n");
        /* Second line — the pipeline stalls — indented to align under
         * "funit" (the "CPU stalls:   " prefix is 14 columns). Full counts
         * make six buckets on one line far too wide for an 80-column term. */
        bench_puts("              hazard ");
        print_count_pct(d_hazard, d_cycles);
        bench_puts(", flush ");
        print_count_pct(d_flush, d_cycles);
        bench_puts("\n");
    }
}

/* ── Cache performance counters ────────────────────────────────── */

void bench_cache_perf_snapshot_l1d(bench_cache_perf_t *out) {
    out->read_hits    = read_sysreg(SYSDEV_L1_DCACHE, CACHE_READ_HITS);
    out->read_misses  = read_sysreg(SYSDEV_L1_DCACHE, CACHE_READ_MISSES);
    out->write_hits   = read_sysreg(SYSDEV_L1_DCACHE, CACHE_WRITE_HITS);
    out->write_misses = read_sysreg(SYSDEV_L1_DCACHE, CACHE_WRITE_MISSES);
}

void bench_cache_perf_snapshot_l1i(bench_cache_perf_t *out) {
    out->read_hits    = read_sysreg(SYSDEV_L1_ICACHE, CACHE_READ_HITS);
    out->read_misses  = read_sysreg(SYSDEV_L1_ICACHE, CACHE_READ_MISSES);
    out->write_hits   = read_sysreg(SYSDEV_L1_ICACHE, CACHE_WRITE_HITS);
    out->write_misses = read_sysreg(SYSDEV_L1_ICACHE, CACHE_WRITE_MISSES);
}

void bench_cache_perf_snapshot_l2(bench_cache_perf_t *out) {
    out->read_hits    = read_sysreg(SYSDEV_L2_CACHE, CACHE_READ_HITS);
    out->read_misses  = read_sysreg(SYSDEV_L2_CACHE, CACHE_READ_MISSES);
    out->write_hits   = read_sysreg(SYSDEV_L2_CACHE, CACHE_WRITE_HITS);
    out->write_misses = read_sysreg(SYSDEV_L2_CACHE, CACHE_WRITE_MISSES);
}

/* Print "<numerator> / <denominator> (<pct>.<frac>%)" where pct is
 * computed in fixed-point with 1 fractional digit.  Both inputs are
 * 32-bit unsigned; the intermediate (numerator * 1000) is kept in
 * 32-bit by pre-scaling if needed.  If denominator is 0, prints
 * "0 / 0 (n/a)" — no division-by-zero, and the caller can tell at
 * a glance that the cache didn't see traffic in this category. */
static void print_hit_rate(uint32_t hits, uint32_t total) {
    bench_print_uint(hits);
    bench_puts(" / ");
    bench_print_uint(total);
    bench_puts(" (");
    if (total == 0) {
        bench_puts("n/a)");
        return;
    }
    /* Compute hit rate * 10 (one fractional digit) in 32-bit.
     * (hits * 1000) overflows if hits >= 2^22 (~4.2M); scale both
     * down by the same shift to preserve the ratio. */
    uint32_t h = hits, t = total;
    while (h > 0x003FFFFFu) {       /* keep h * 1000 < 2^32 */
        h >>= 1;
        t >>= 1;
    }
    uint32_t pct_x10 = (t > 0) ? (h * 1000) / t : 0;
    bench_print_uint(pct_x10 / 10);
    bench_putchar('.');
    bench_print_uint(pct_x10 % 10);
    bench_puts("%)");
}

void bench_cache_perf_print_delta(const char *label,
                                  const bench_cache_perf_t *before,
                                  const bench_cache_perf_t *after) {
    /* Modular subtraction is wrap-safe across the 32-bit boundary. */
    uint32_t d_rh = after->read_hits    - before->read_hits;
    uint32_t d_rm = after->read_misses  - before->read_misses;
    uint32_t d_wh = after->write_hits   - before->write_hits;
    uint32_t d_wm = after->write_misses - before->write_misses;

    bench_puts(label);
    bench_puts(": read ");
    print_hit_rate(d_rh, d_rh + d_rm);
    if ((d_wh + d_wm) > 0) {
        bench_puts(", write ");
        print_hit_rate(d_wh, d_wh + d_wm);
    }
    bench_puts("\n");
}

/* ── Console output ────────────────────────────────────────────── */

void bench_putchar(int c) {
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (uint32_t)c;
}

void bench_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            bench_putchar('\r');
        bench_putchar(*s++);
    }
}

void bench_print_uint(uint32_t val) {
    char buf[12];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val) {
            *--p = '0' + (val % 10);
            val /= 10;
        }
    }
    bench_puts(p);
}

void bench_print_int(int val) {
    if (val < 0) {
        bench_putchar('-');
        bench_print_uint((uint32_t)(-val));
    } else {
        bench_print_uint((uint32_t)val);
    }
}

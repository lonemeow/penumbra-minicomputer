/*
 * bench.h — Penumbra bare-metal benchmark harness
 *
 * Provides timer measurement and console output for benchmarks
 * running directly from the ROM boot command (no OS).
 */

#ifndef BENCH_H
#define BENCH_H

typedef unsigned int uint32_t;

/*
 * Initialize the benchmark harness: start the periodic timer
 * and enable interrupts for overflow counting.
 * Call once before any timing measurements.
 */
void bench_init(void);

/*
 * Reset the timer for a new measurement.
 * Clears the overflow counter and restarts the 16-bit countdown.
 */
void bench_timer_start(void);

/*
 * Return elapsed raw ticks since the last bench_timer_start().
 * Cheap: one add + one subtract, no multiply or divide.
 * Use this in hot loops; convert to µs once at the end.
 */
uint32_t bench_timer_elapsed_ticks(void);

/*
 * Return the number of ticks corresponding to the given number of
 * microseconds.  Use to precompute a deadline for polling loops:
 *   uint32_t deadline = bench_us_to_ticks(2000000);  // 2 seconds
 *   while (bench_timer_elapsed_ticks() < deadline) { ... }
 */
uint32_t bench_us_to_ticks(uint32_t us);

/*
 * Return elapsed microseconds since the last bench_timer_start().
 * Involves a software divide — call sparingly (start/end of run),
 * not in tight polling loops.
 */
uint32_t bench_timer_elapsed_us(void);

/*
 * Return the timer tick frequency in Hz (set by bench_init).
 * Useful for computing derived metrics (e.g. ticks / freq = seconds).
 */
uint32_t bench_timer_freq_hz(void);

/*
 * Enable / disable both data and instruction caches.
 * crt0.S enables them by default after MMU setup.  Memtest and any
 * other benchmark that needs to bypass the cache and exercise raw
 * memory should call bench_caches_disable() at entry to bench_main.
 *
 * Disabling the caches is a master override: pages mapped with PTE.C
 * still go through to memory because cache_active = enable & PTE.C.
 * Per-page uncached mappings (PTE_KERNEL_NC) are the finer-grained
 * alternative when only part of the address space should bypass.
 */
void bench_caches_enable(void);
void bench_caches_disable(void);

/*
 * CPU performance counter snapshot.
 * Free-running 32-bit counters from SYSDEV_CPU.  Wraps every
 * ~5.7 minutes at 12.5 MHz; benchmarks take seconds, so deltas are safe.
 */
typedef struct {
    uint32_t cycles;
    uint32_t insns_retired;
} bench_perf_t;

/*
 * Snapshot the CPU performance counters into `out`.  Two RDSYS
 * instructions, executed one cycle apart — for normal benchmark
 * workloads (millions of cycles) the inter-counter skew is negligible.
 */
void bench_perf_snapshot(bench_perf_t *out);

/*
 * Print "<label>: <Δcycles> cycles, <Δinsns> insns, CPI=X.YYY"
 * to the console.  Computes deltas (after - before) modulo 32-bit wrap.
 */
void bench_perf_print_delta(const char *label,
                            const bench_perf_t *before,
                            const bench_perf_t *after);

/*
 * Cache performance counter snapshot.
 * Free-running 32-bit counters from each cache device's regs 10-13.
 * Layout is identical for L1-D, L1-I, L2, and any future cache slot;
 * the parent cache instance is selected by the snapshot function.
 * Wraps every ~170 s at 25 MHz; benchmarks run in seconds so
 * deltas are safe.
 */
typedef struct {
    uint32_t read_hits;
    uint32_t read_misses;
    uint32_t write_hits;
    uint32_t write_misses;
} bench_cache_perf_t;

/*
 * Snapshot one cache's perfctrs.  RDSYS encodes the device id in
 * the instruction (no runtime indirection), so we expose a
 * per-instance function rather than one taking a runtime dev id.
 */
void bench_cache_perf_snapshot_l1d(bench_cache_perf_t *out);
void bench_cache_perf_snapshot_l1i(bench_cache_perf_t *out);
void bench_cache_perf_snapshot_l2 (bench_cache_perf_t *out);

/*
 * Print a labeled delta line with the four raw counter deltas plus
 * computed read- and write-hit-rates.  Skips the write section if
 * the cache saw zero writes (I-caches never see stores).
 */
void bench_cache_perf_print_delta(const char *label,
                                  const bench_cache_perf_t *before,
                                  const bench_cache_perf_t *after);

/* Console output (polled UART at 0xFF000000) */
void bench_putchar(int c);
void bench_puts(const char *s);
void bench_print_uint(uint32_t val);
void bench_print_int(int val);

#endif /* BENCH_H */

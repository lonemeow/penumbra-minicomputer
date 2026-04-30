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

/* Console output (polled UART at 0xFF000000) */
void bench_putchar(int c);
void bench_puts(const char *s);
void bench_print_uint(uint32_t val);
void bench_print_int(int val);

#endif /* BENCH_H */

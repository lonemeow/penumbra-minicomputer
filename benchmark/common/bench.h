/*
 * bench.h — Penumbra bare-metal benchmark harness
 *
 * Provides timer measurement and console output for benchmarks
 * running directly from the ROM boot command (no OS).
 */

#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

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
 * Map [base, limit) cached even where it falls in the MMIO range the
 * TLB miss handler otherwise maps uncached.  For a device aperture
 * that behaves as plain memory — a framebuffer whose only writer is
 * the CPU — where a cached copy cannot go stale; never for registers
 * the device itself updates.
 *
 * Flushes the main TLB, since entries installed under the previous
 * range still carry its cacheability.  Pass (0, 0) to restore the
 * default, under which every device is uncached.
 *
 * Lives in tlbrange.c, which depends on the handler crt0.S installs —
 * link that object to use this.
 */
void bench_map_cached(uint32_t base, uint32_t limit);

/*
 * CPU performance counter snapshot.
 * Free-running 32-bit counters from SYSDEV_CPU.  Wraps every
 * ~5.7 minutes at 12.5 MHz; benchmarks take seconds, so deltas are safe.
 */
typedef struct {
    uint32_t cycles;
    uint32_t insns_retired;
    uint32_t stall_funit;    /* cycles stalled on a multi-cycle exec unit */
    uint32_t stall_ifetch;   /* cycles stalled on instruction fetch       */
    uint32_t stall_load;     /* cycles stalled on a data read miss-fill   */
    uint32_t stall_store;    /* cycles stalled on a data write            */
    uint32_t stall_hazard;   /* cycles stalled by a pipeline interlock    */
    uint32_t stall_flush;    /* cycles lost to a front-end redirect/fill  */
} bench_perf_t;

/*
 * Snapshot the CPU performance counters into `out`.  Two RDSYS
 * instructions, executed one cycle apart — for normal benchmark
 * workloads (millions of cycles) the inter-counter skew is negligible.
 */
void bench_perf_snapshot(bench_perf_t *out);

/*
 * Print "<label>: <Δcycles> cycles, <Δinsns> insns, CPI=X.YYY"
 * to the console, followed — when any stall was recorded — by the
 * six-bucket stall breakdown (funit/ifetch/load/store on one line,
 * hazard/flush on a second), each bucket's cycle count and its share of
 * total cycles. The breakdown is omitted when all buckets are zero (e.g.
 * on the ISS, which is instruction-accurate and reports no stalls).
 * Computes deltas (after - before) modulo 32-bit wrap.
 */
void bench_perf_print_delta(const char *label,
                            const bench_perf_t *before,
                            const bench_perf_t *after);

/*
 * Cache geometry, decoded from a cache device's INFO register.
 * `bytes` is the total data the cache holds — the size a working set
 * has to exceed for its accesses to reach the next level down.
 * `present` is false when the device id is unmapped on this bitstream,
 * which is how a variant without an L2 reports itself.
 */
typedef struct {
    int      present;
    uint32_t line_words;
    uint32_t num_sets;
    uint32_t num_ways;
    uint32_t bytes;
} bench_cache_geom_t;

void bench_cache_geom_l1d(bench_cache_geom_t *out);
void bench_cache_geom_l2 (bench_cache_geom_t *out);

/*
 * The outermost cache a load can hit — L2 where there is one, else
 * L1-D.  A benchmark that means to reach memory sizes its working set
 * from this rather than from a constant, so it stays honest on a
 * machine whose hierarchy it was not written against.
 */
void bench_cache_geom_outermost(bench_cache_geom_t *out);

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

/*
 * Machine identity: the board's name, the CPU implementation's name,
 * and the CPU clock.  A name is the register's 16 characters and a
 * terminator.  The clock reads 0 on a machine that reports none.
 */
#define BENCH_NAME_LEN 17

void     bench_machine_name(char name[BENCH_NAME_LEN]);
void     bench_cpu_name(char name[BENCH_NAME_LEN]);
uint32_t bench_cpu_freq_hz(void);

/*
 * A machine's main memory, as a benchmark report has to state it.  A
 * score divided by the CPU clock is only comparable alongside what the
 * memory is and how its clock stands to the CPU's, and no register
 * says either, so the harness keeps them by machine name.
 */
typedef struct {
    char     machine[BENCH_NAME_LEN];   /* the machine name this describes */
    char     memory[32];                /* memory type and clock, as printed */
    uint32_t clock_hz;                  /* that clock, for its ratio to the CPU's */
    int      simulated;                 /* a simulator: its timings are not
                                         * measurements, and it has no memory
                                         * to describe */
} bench_memory_t;

/*
 * The description of the machine this is running on, or 0 for a
 * machine the harness has not been told about — which a report should
 * say out loud, never guess around.
 */
const bench_memory_t *bench_machine_memory(void);

/* Console output (polled UART at 0xFF000000) */
void bench_putchar(int c);
void bench_puts(const char *s);
void bench_print_uint(uint32_t val);
void bench_print_int(int val);

/* Print a value held in thousandths, to three decimals: 56123 prints
 * as "56.123". */
void bench_print_milli(uint64_t milli);

#endif /* BENCH_H */

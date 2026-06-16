/* perfctr.h - optional per-benchmark CPU + cache performance-counter dump.
 *
 * RDSYS is privileged, so userland reads the free-running counters through
 * the kernel's machdep.cpu.* / machdep.cache.* sysctls (same source penmon
 * uses).  When pbench's -p flag is set, bench_time() brackets one extra
 * f(iters) batch with a snapshot pair and prints the breakdown — the stall
 * attribution and cache hit rates that show where a benchmark's cycles go.
 * Off by default so normal benchmarking output stays clean.
 */
#ifndef PBENCH_PERFCTR_H
#define PBENCH_PERFCTR_H

#include <stdint.h>
#include <machine/sysreg.h>   /* CPU_PERF_*, CACHE_PERF_*, CPU/CACHE_NPERFCTR */

/* Set by pbench's -p flag. */
extern int bench_perfctr_enabled;

/* One reading of every CPU + cache counter, in bulk-sysctl index order
 * (CPU_PERF_* / CACHE_PERF_*).  ok is 0 if any sysctl was missing — e.g. a
 * kernel built without the perfctr leaves — so the report degrades to a
 * one-line notice instead of printing garbage. */
struct perf_snapshot {
    uint64_t cpu[CPU_NPERFCTR];
    uint64_t l1i[CACHE_NPERFCTR];
    uint64_t l1d[CACHE_NPERFCTR];
    uint64_t l2[CACHE_NPERFCTR];
    int      ok;
};

/* Read all counters into *s. */
void perf_snapshot_take(struct perf_snapshot *s);

/* Print the CPU stall breakdown + cache hit rates between two snapshots,
 * indented under the benchmark's result row.  iters is the batch's
 * iteration count, used to show cycles-per-op. */
void perf_report(const struct perf_snapshot *before,
                 const struct perf_snapshot *after, uint64_t iters);

/* For standalone compute demos: snapshot the counters now (call at the
 * demo's render-start, after setup) and print the CPU/cache breakdown at
 * program exit via atexit(), after the demo's own framerate stats. */
void perf_demo_track(void);

#endif /* PBENCH_PERFCTR_H */

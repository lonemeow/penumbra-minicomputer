/* perfctr.c - per-benchmark CPU + cache performance-counter dump.
 *
 * Reads the kernel's bulk perfctr sysctls (one syscall per device) and
 * prints the stall attribution + cache hit rates for a benchmark batch.
 * See perfctr.h for why this goes through sysctl rather than RDSYS.
 */
#include "perfctr.h"

#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>		/* atexit */

int bench_perfctr_enabled = 0;

/* Read a uint64_t[n] bulk-counter sysctl.  Returns 1 on a full read. */
static int read_bulk(const char *name, uint64_t *v, size_t n)
{
    size_t len = n * sizeof(uint64_t);
    return sysctlbyname(name, v, &len, NULL, 0) == 0 && len == n * sizeof(uint64_t);
}

void perf_snapshot_take(struct perf_snapshot *s)
{
    s->ok  = read_bulk("machdep.cpu.all",       s->cpu, CPU_NPERFCTR);
    s->ok &= read_bulk("machdep.cache.l1i.all", s->l1i, CACHE_NPERFCTR);
    s->ok &= read_bulk("machdep.cache.l1d.all", s->l1d, CACHE_NPERFCTR);
    s->ok &= read_bulk("machdep.cache.l2.all",  s->l2,  CACHE_NPERFCTR);
}

/* Counters are free-running 32-bit, zero-extended into 64 by the kernel;
 * the true interval delta is the unsigned difference modulo 2^32. */
static uint64_t cdelta(uint64_t prev, uint64_t cur)
{
    return (cur - prev) & 0xFFFFFFFFull;
}

static double pct(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}

/* "<tag> hit NN.N%" — combined read+write hit rate for one cache. */
static void cache_hit(const char *tag, const uint64_t *b, const uint64_t *a)
{
    uint64_t rh = cdelta(b[CACHE_PERF_READ_HITS],    a[CACHE_PERF_READ_HITS]);
    uint64_t rm = cdelta(b[CACHE_PERF_READ_MISSES],  a[CACHE_PERF_READ_MISSES]);
    uint64_t wh = cdelta(b[CACHE_PERF_WRITE_HITS],   a[CACHE_PERF_WRITE_HITS]);
    uint64_t wm = cdelta(b[CACHE_PERF_WRITE_MISSES], a[CACHE_PERF_WRITE_MISSES]);
    printf("%s hit %4.1f%%  ", tag, pct(rh + wh, rh + wh + rm + wm));
}

/* Shorthand: a CPU stall bucket's share of the batch's cycles. */
static double stall_pct(const struct perf_snapshot *b, const struct perf_snapshot *a,
                        int idx, uint64_t cyc)
{
    return pct(cdelta(b->cpu[idx], a->cpu[idx]), cyc);
}

void perf_report(const struct perf_snapshot *before,
                 const struct perf_snapshot *after, uint64_t iters)
{
    if (!before->ok || !after->ok) {
        printf("perfctr unavailable (no machdep.cpu/cache.* leaves)\n");
        return;
    }

    uint64_t cyc = cdelta(before->cpu[CPU_PERF_CYCLES], after->cpu[CPU_PERF_CYCLES]);
    uint64_t ins = cdelta(before->cpu[CPU_PERF_INSNS],  after->cpu[CPU_PERF_INSNS]);

    if (iters)
        printf("perfctr CPI %.2f, %llu cyc/op  (%llu cyc / %llu insn)\n",
               ins ? (double)cyc / (double)ins : 0.0,
               (unsigned long long)(cyc / iters),
               (unsigned long long)cyc, (unsigned long long)ins);
    else
        printf("perfctr CPI %.2f  (%llu cyc / %llu insn)\n",
               ins ? (double)cyc / (double)ins : 0.0,
               (unsigned long long)cyc, (unsigned long long)ins);

    /* Unindented, all six buckets fit one 80-column line. */
    printf("stalls  funit %4.1f%% ifetch %4.1f%% load %4.1f%% store %4.1f%%"
           " hazard %4.1f%% flush %4.1f%%\n",
           stall_pct(before, after, CPU_PERF_STALL_FUNIT,  cyc),
           stall_pct(before, after, CPU_PERF_STALL_IFETCH, cyc),
           stall_pct(before, after, CPU_PERF_STALL_LOAD,   cyc),
           stall_pct(before, after, CPU_PERF_STALL_STORE,  cyc),
           stall_pct(before, after, CPU_PERF_STALL_HAZARD, cyc),
           stall_pct(before, after, CPU_PERF_STALL_FLUSH,  cyc));

    printf("cache   ");
    cache_hit("L1I", before->l1i, after->l1i);
    cache_hit("L1D", before->l1d, after->l1d);
    cache_hit("L2",  before->l2,  after->l2);
    printf("\n");
}

/* --- Demo exit dump ------------------------------------------------- */
/* A standalone compute demo can't call bench_time(); instead it snapshots
 * at its render-start and prints the breakdown at exit, after its own
 * framerate stats.  Used by the graphics demos to show where a CPU-heavy,
 * OS-light workload spends its cycles. */

static struct perf_snapshot perf_demo_s0;

static void
perf_demo_atexit(void)
{
    struct perf_snapshot s1;
    perf_snapshot_take(&s1);
    perf_report(&perf_demo_s0, &s1, 0);
}

void
perf_demo_track(void)
{
    perf_snapshot_take(&perf_demo_s0);
    atexit(perf_demo_atexit);
}

/* bench.c - pbench harness implementation: timing, calibration, reduction,
 * result emission.
 *
 * The harness reads CLOCK_MONOTONIC and aims for ~50 ms per trial.  Each
 * trial's wall-clock is divided by the iteration count to get ns/op; min,
 * median and mean across BENCH_NUM_TRIALS trials are reported.
 *
 * Min is the headline number for microbenchmarks: it represents the
 * lowest-noise observation (no preemption, hot caches, lucky scheduler
 * alignment).  Median tells you about jitter — if median >> min, something
 * is interfering systematically (timer interrupts, page faults).
 */

#include "bench.h"
#include "perfctr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* --- Output target --------------------------------------------------- */

/* Stream for machine-readable RESULT lines.  NULL means "don't emit". */
static FILE *result_file = NULL;

void bench_set_result_file(void *fp) {
    result_file = (FILE *)fp;
}

/* --- Time helpers ---------------------------------------------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC is the right choice: not affected by settimeofday,
     * counts from an arbitrary epoch, guaranteed monotonic. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --- Trial reduction ------------------------------------------------ */

/* Insertion sort over a small uint64_t array.  For N up to a few dozen
 * (BENCH_MAX_TRIALS is 50) this is faster than qsort and shorter than
 * the comparator + qsort call. */
static void sort_u64(uint64_t *v, size_t n) {
    for (size_t i = 1; i < n; i++) {
        uint64_t x = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* --- Public API ------------------------------------------------------ */

int bench_time(const char *category, const char *name, const char *config,
               bench_fn f, void *ctx)
{
    uint64_t trials[BENCH_MAX_TRIALS];

    /* Step 1: Throwaway.  One untimed call primes I-cache, D-cache,
     * TLB, branch predictor, and any kernel/libc lazy-init paths so
     * the first calibration probe isn't dominated by cold-start cost. */
    f(1, ctx);

    /* Step 2: Calibrate iters by doubling.  Each probe is a SINGLE
     * timed call to f(probe).  No inner clock-bracketed loop — that
     * was the v2 trap: now_ns() inside the warmup condition added
     * ~1 ms per warmup iter, which inflated per_iter and gave us
     * iters=13 instead of the expected ~90.
     *
     * Doubling stops as soon as a single probe call takes
     * >= BENCH_TARGET_TRIAL_NS.  At that point iters = probe is
     * exactly the count where each trial is well above the noise
     * floor; no scaling division required. */
    uint64_t iters = 1;
    for (uint64_t probe = 1; probe <= BENCH_MAX_PROBE_ITERS; probe *= 2) {
        bench_compiler_barrier();
        uint64_t t0 = now_ns();
        f(probe, ctx);
        uint64_t t1 = now_ns();
        bench_compiler_barrier();
        uint64_t elapsed = (t1 > t0) ? (t1 - t0) : 0;
        iters = probe;
        if (elapsed >= BENCH_TARGET_TRIAL_NS)
            break;
    }

    /* Step 3: Run trials.  Variable count: keep timing f(iters)
     * until either the trial budget is exhausted (with at least
     * BENCH_MIN_TRIALS done) or we hit the absolute cap.
     *
     * Fast ops: each trial is ~target_ns, so ~3 s budget gives
     * ~15 trials — far better statistics than the old fixed 5.
     * Slow ops: f(1) already exceeds target, so each trial is one
     * full op; loop exits at MIN_TRIALS because the budget was
     * blown on the first or second trial. */
    int n_trials = 0;
    uint64_t trials_start = now_ns();
    do {
        bench_compiler_barrier();
        uint64_t t0 = now_ns();
        f(iters, ctx);
        uint64_t t1 = now_ns();
        bench_compiler_barrier();
        /* Same underflow guard as in calibration. */
        trials[n_trials++] = (t1 > t0) ? (t1 - t0) : 0;
    } while (n_trials < BENCH_MAX_TRIALS &&
             (n_trials < BENCH_MIN_TRIALS ||
              (now_ns() - trials_start) < BENCH_TRIALS_BUDGET_NS));

    /* Step 4: Reduce. */
    sort_u64(trials, n_trials);
    double min_ns_per = (double)trials[0] / (double)iters;
    double med_ns_per = (double)trials[n_trials / 2] / (double)iters;
    uint64_t sum = 0;
    for (int i = 0; i < n_trials; i++) sum += trials[i];
    double mean_ns_per = (double)sum / ((double)n_trials * (double)iters);

    /* Emit.  Two forms — human-readable table row, machine-readable RESULT. */
    const char *cfg_show = config ? config : "-";

    /* Pick a display unit from the median value so all three columns of
     * this row use the same scale — easier to eyeball than mixed units. */
    const char *unit;
    double scale;
    if (med_ns_per < 1e3)      { unit = "ns"; scale = 1.0;   }
    else if (med_ns_per < 1e6) { unit = "us"; scale = 1e3;   }
    else if (med_ns_per < 1e9) { unit = "ms"; scale = 1e6;   }
    else                       { unit = "s";  scale = 1e9;   }

    /* Human row: name padded, config padded, min/med/mean right-aligned.
     * Two decimals is enough — more is noise from a 1-trial sample. */
    printf("  %-8s %-18s %-14s  min=%9.2f %-2s  med=%9.2f %-2s  mean=%9.2f %-2s  iters=%llu  trials=%d\n",
           category, name, cfg_show,
           min_ns_per / scale, unit,
           med_ns_per / scale, unit,
           mean_ns_per / scale, unit,
           (unsigned long long)iters, n_trials);

    /* Machine line: greppable, key=value, stable column ordering.  Only
     * emitted if the caller registered an output file via
     * bench_set_result_file() — keeps stdout clean for blog-friendly
     * pasting by default. */
    if (result_file) {
        fprintf(result_file,
                "RESULT category=%s name=%s config=%s "
                "min_ns=%.3f median_ns=%.3f mean_ns=%.3f iters=%llu trials=%d\n",
                category, name, cfg_show,
                min_ns_per, med_ns_per, mean_ns_per,
                (unsigned long long)iters, n_trials);
        fflush(result_file);
    }

    fflush(stdout);

    /* Optional: one extra f(iters) batch bracketed by a perfctr snapshot
     * pair, so a -p run shows where this benchmark's cycles went.  Kept out
     * of the timed trials above so it never perturbs the reported ns/op. */
    if (bench_perfctr_enabled) {
        struct perf_snapshot pb, pa;
        bench_compiler_barrier();
        perf_snapshot_take(&pb);
        f(iters, ctx);
        perf_snapshot_take(&pa);
        bench_compiler_barrier();
        perf_report(&pb, &pa, iters);
        fflush(stdout);
    }

    return 0;
}

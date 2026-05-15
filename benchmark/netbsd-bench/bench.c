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

/* --- Calibration: find iteration count that gives ~target_ns per trial. */

#define BENCH_CALIBRATE_MAX         (1ull << 30)  /* 1B iterations */
#define BENCH_CALIBRATE_BUDGET_NS   (500ull * 1000ull * 1000ull)  /* 500 ms */
#define BENCH_CALIBRATE_TARGET_NS   (50ull * 1000ull * 1000ull) /* 50ms */

/* Don't trust elapsed measurements below this — on Penumbra a single
 * clock_gettime syscall is ~1 ms, so anything under 10 ms is noise
 * dominated by the syscalls bracketing f().  Only update ns_per_iter
 * once elapsed is comfortably above that floor. */
#define BENCH_CALIBRATE_TRUST_NS    (BENCH_CALIBRATE_TARGET_NS / 5)  /* 10ms */

static uint64_t bench_calibrate(bench_fn f, void *ctx, uint64_t target_ns) {
    uint64_t deadline = now_ns() + BENCH_CALIBRATE_BUDGET_NS;
    uint64_t ns_per_iter = 1;

    for (uint64_t iters = 1; iters < BENCH_CALIBRATE_MAX; iters *= 2) {
        uint64_t estimated_end = now_ns() + ns_per_iter * iters;
        if (estimated_end > deadline)
            break;

        uint64_t start = now_ns();
        f(iters, ctx);
        uint64_t end = now_ns();
        /* Defensive: CLOCK_MONOTONIC should be monotonic, but a stale
         * timecounter read across a context switch has been seen to
         * tick backwards by a few units.  Treat that as zero rather
         * than letting uint64 underflow inflate the mean to ~2^64. */
        uint64_t elapsed = (end > start) ? (end - start) : 0;

        /* Only believe the measurement once it's clearly above clock
         * noise.  Otherwise keep ns_per_iter pessimistic so the loop
         * keeps doubling. */
        if (elapsed >= BENCH_CALIBRATE_TRUST_NS) {
            ns_per_iter = elapsed / iters;
            if (ns_per_iter == 0) ns_per_iter = 1;
            if (elapsed >= target_ns) break;
        }
    }

    if (ns_per_iter == 0) ns_per_iter = 1;
    return target_ns / ns_per_iter;
}

/* --- Trial reduction ------------------------------------------------ */

/* Insertion sort over a small uint64_t array.  For N=BENCH_NUM_TRIALS (5)
 * this is faster than qsort and shorter than the comparator + qsort call. */
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
    uint64_t trials[BENCH_NUM_TRIALS];

    /* Warmup: prime caches, page tables, branch state (such as it is). */
    f(BENCH_WARMUP_ITERS, ctx);

    /* Calibrate: find iteration count for ~50ms per trial. */
    uint64_t iters = bench_calibrate(f, ctx, BENCH_TARGET_TRIAL_NS);
    if (iters < 1) iters = 1;

    /* Measure. */
    for (int i = 0; i < BENCH_NUM_TRIALS; i++) {
        bench_compiler_barrier();
        uint64_t t0 = now_ns();
        f(iters, ctx);
        uint64_t t1 = now_ns();
        bench_compiler_barrier();
        /* Same underflow guard as in calibration. */
        trials[i] = (t1 > t0) ? (t1 - t0) : 0;
    }

    /* Reduce. */
    sort_u64(trials, BENCH_NUM_TRIALS);
    double min_ns_per = (double)trials[0] / (double)iters;
    double med_ns_per = (double)trials[BENCH_NUM_TRIALS / 2] / (double)iters;
    uint64_t sum = 0;
    for (int i = 0; i < BENCH_NUM_TRIALS; i++) sum += trials[i];
    double mean_ns_per = (double)sum / ((double)BENCH_NUM_TRIALS * (double)iters);

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
    printf("  %-8s %-18s %-14s  min=%9.2f %-2s  med=%9.2f %-2s  mean=%9.2f %-2s  iters=%llu\n",
           category, name, cfg_show,
           min_ns_per / scale, unit,
           med_ns_per / scale, unit,
           mean_ns_per / scale, unit,
           (unsigned long long)iters);

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
                (unsigned long long)iters, BENCH_NUM_TRIALS);
        fflush(result_file);
    }

    fflush(stdout);
    return 0;
}

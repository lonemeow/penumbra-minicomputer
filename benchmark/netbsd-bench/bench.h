/* bench.h - Penumbra pbench harness: public API.
 *
 * Each benchmark module exposes one entry function and registers it in
 * pbench.c's static registry array.  The entry function calls bench_time()
 * one or more times; bench_time() handles auto-calibration, warmup, trial
 * looping, statistical reduction, and result emission.
 */

#ifndef PBENCH_BENCH_H
#define PBENCH_BENCH_H

#include <stdint.h>
#include <stddef.h>

/* Auto-calibration target: each trial aims for this wall-clock duration.
 * Big enough that timer overhead is irrelevant; small enough that 5 trials
 * complete in well under a second on the ISS. */
#define BENCH_TARGET_TRIAL_NS  (50ull * 1000ull * 1000ull)  /* 50 ms */
#define BENCH_NUM_TRIALS       5
#define BENCH_WARMUP_ITERS     16

/* Benchmark work function: called `iters` times in a tight inner loop by
 * the harness, with `ctx` passed unchanged.  Implementations should *not*
 * read the clock themselves — the harness times the whole batch. */
typedef void (*bench_fn)(uint64_t iters, void *ctx);

/* Set the file to which machine-readable RESULT lines are written.
 * If never called (or called with NULL), only the human-readable table
 * row is emitted.  Set from main() once after parsing args. */
void bench_set_result_file(void *fp);

/* Run a benchmark and emit results.  Returns 0 on success.
 *
 *   category   "kernel" or "libc" (used for grouping and filtering)
 *   name       short identifier, e.g. "getpid" or "memcpy"
 *   config     optional sub-configuration string ("size=64") or NULL
 *   f          work function — must execute `iters` distinct units of work
 *   ctx        opaque pointer passed to f
 *
 * The harness:
 *   1. Calls f(BENCH_WARMUP_ITERS, ctx) once to warm caches/TLB.
 *   2. Calibrates: finds N such that f(N, ctx) takes ~BENCH_TARGET_TRIAL_NS.
 *   3. Runs BENCH_NUM_TRIALS trials of f(N, ctx), recording elapsed ns each.
 *   4. Reduces to min/median/mean ns-per-iter and emits human + machine lines. */
int bench_time(const char *category, const char *name, const char *config,
               bench_fn f, void *ctx);

/* Registry entry — one per benchmark, listed in pbench.c. */
struct bench_entry {
    const char *category;       /* "kernel" or "libc" */
    const char *name;           /* unique within category */
    void (*run)(void);          /* benchmark entry; calls bench_time() */
};

/* Force the compiler to treat a value as observed.  Prevents the optimizer
 * from deleting dead stores / proving loop-invariant computations.  Used by
 * libc microbenchmarks to keep memcpy/memset/etc. from being elided. */
static inline void bench_consume(const volatile void *p) {
    (void)p;
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/* Compiler memory barrier; forces all pending loads/stores to be emitted
 * before subsequent operations.  Used around timed regions. */
static inline void bench_compiler_barrier(void) {
    __asm__ __volatile__("" : : : "memory");
}

/* Place INSIDE the inner loop of a benchmark, right after the operation
 * under measurement.  The "memory" clobber tells the compiler that some
 * unknown code may have modified arbitrary memory between iterations —
 * which forces re-evaluation of pure-function calls (memcpy, memset,
 * strlen, getpid …) that clang would otherwise CSE across the loop.
 * Without this, "for (i; iters; i++) strlen(s);" collapses to one
 * strlen call followed by iters cheap additions. */
static inline void bench_clobber(void) {
    __asm__ __volatile__("" : : : "memory");
}

#endif /* PBENCH_BENCH_H */

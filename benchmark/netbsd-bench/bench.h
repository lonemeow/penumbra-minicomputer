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

/* Self-exec support for spawn benchmarks.
 *
 * fork_exec measures process spawn by re-execing *this same binary* with
 * a sentinel argument, so it needs no external target and automatically
 * matches its own link mode (the static pbench execs static, dynamic
 * execs dynamic).  main() recognizes PBENCH_EXEC_CHILD_ARG as argv[1] and
 * _exit()s immediately, and records the path to itself in pbench_self_path
 * for the benchmark to execve(). */
#define PBENCH_EXEC_CHILD_ARG "__exec_child"
extern const char *pbench_self_path;

/* Each trial is one timed call to f(iters).  Calibration picks iters
 * such that this call takes at least BENCH_TARGET_TRIAL_NS — long
 * enough that `clock_gettime` overhead (~1–5 ms on Penumbra) is a
 * small fraction of the trial AND random timer-interrupt jitter
 * averages out across the trial's iterations. */
#define BENCH_TARGET_TRIAL_NS    (200ull * 1000ull * 1000ull)            /* 200 ms */

/* Trial loop runs until either MIN trials AND budget exhausted, or
 * MAX cap hit.  Variable trial count: fast ops fit many trials in the
 * budget (better statistics); slow ops hit the MIN floor.  No fixed
 * minimum on iters per trial — slow ops legitimately use iters=1. */
#define BENCH_TRIALS_BUDGET_NS   (5ull * 1000ull * 1000ull * 1000ull)    /* 5 s   */
#define BENCH_MIN_TRIALS         5
#define BENCH_MAX_TRIALS         50

/* Cap on the calibration doubling.  Prevents pathological calibration
 * runaway if f(iters) somehow always reads as zero (e.g., a benchmark
 * stub that returns immediately). */
#define BENCH_MAX_PROBE_ITERS    (1ull << 28)                            /* 256 M */

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
 *   1. Calls f(1, ctx) once to prime caches/TLB (untimed throwaway).
 *   2. Calibrates by doubling: f(1), f(2), f(4), …, stopping as soon
 *      as a single call takes >= BENCH_TARGET_TRIAL_NS.  Each probe is
 *      one timed measurement; no clock-bracketed inner loop adds
 *      overhead to the per-iter estimate.  iters is the probe count
 *      at exit.
 *   3. Runs trials of f(iters, ctx), recording elapsed ns each.  Loop
 *      continues until either BENCH_MAX_TRIALS reached, or
 *      BENCH_MIN_TRIALS reached AND BENCH_TRIALS_BUDGET_NS exhausted.
 *      Slow ops (call already >= target) end up at iters=1 with the
 *      MIN_TRIALS floor; fast ops get many more trials within budget.
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

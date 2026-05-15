/* libc/qsort_int.c - qsort(3) of N ints; composite signal.
 *
 * Exercises three things at once: libc's qsort implementation (typically
 * a hybrid quicksort/insertion-sort), the indirect-call cost on each
 * comparison (very expensive on Penumbra — no branch predictor, every
 * call pays full pipeline-refill), and memory access patterns over the
 * working set.
 *
 * Each iteration starts from a pristine random array.  We pre-generate
 * the random data once and memcpy it into the working buffer before each
 * qsort call; that adds a fixed O(N) memcpy cost to each iteration, but
 * keeps the sort cost stable (sorting already-sorted data is a degenerate
 * case for quicksort).
 *
 * Two sizes: 256 (fits in 1 KiB cache) and 4096 (overflows, exercises
 * SDRAM).  The ratio between the two tells you about cache-vs-memory
 * effects on the sort.
 */

#include "../bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*cmp_fn)(const void *, const void *);

static int cmp_int(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

/* Hide the comparator behind a volatile pointer so the compiler can't
 * inline it into qsort (which it generally can't anyway, but defence in
 * depth — and it stops -fdevirtualize from being clever). */
static volatile cmp_fn vcmp = cmp_int;

struct qs_ctx {
    int *work;
    const int *pristine;
    size_t n;
};

static void run_qsort(uint64_t iters, void *vctx) {
    struct qs_ctx *c = vctx;
    cmp_fn cmp = vcmp;
    for (uint64_t i = 0; i < iters; i++) {
        memcpy(c->work, c->pristine, c->n * sizeof(int));
        qsort(c->work, c->n, sizeof(int), cmp);
    }
    bench_consume(c->work);
}

static const size_t sweep_sizes[] = { 256, 4096 };

void bench_libc_qsort_int(void) {
    size_t max = sweep_sizes[sizeof(sweep_sizes) / sizeof(sweep_sizes[0]) - 1];
    int *pristine = malloc(max * sizeof(int));
    int *work = malloc(max * sizeof(int));
    if (!pristine || !work) { perror("malloc"); return; }

    /* Fixed-seed LCG so runs are comparable across builds.  Don't use
     * rand(3) — its state would be modified by other benchmarks running
     * in the same process. */
    uint32_t s = 0x12345678;
    for (size_t i = 0; i < max; i++) {
        s = s * 1103515245u + 12345u;
        pristine[i] = (int)s;
    }

    for (size_t i = 0; i < sizeof(sweep_sizes) / sizeof(sweep_sizes[0]); i++) {
        char cfg[32];
        snprintf(cfg, sizeof(cfg), "n=%zu", sweep_sizes[i]);
        struct qs_ctx ctx = { .work = work, .pristine = pristine, .n = sweep_sizes[i] };
        bench_time("libc", "qsort_int", cfg, run_qsort, &ctx);
    }

    free(pristine);
    free(work);
}

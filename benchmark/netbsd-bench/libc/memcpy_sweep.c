/* libc/memcpy_sweep.c - memcpy(3) cost vs. buffer size.
 *
 * Sweeps from 1B to 64KiB.  At small sizes (1-16B) you're measuring the
 * per-call overhead — function prologue, size dispatch, return.  Around
 * the cache size (1 KiB on Penumbra) the throughput cliff appears — every
 * subsequent copy refills the line from SDRAM through the CDC bridge.
 * At 64K you're measuring steady-state SDRAM streaming.
 *
 * The function pointer is stashed in a volatile to prevent clang from
 * recognising memcpy(constant_size) and lowering it inline.  We want to
 * measure libc's memcpy — the real one in libc.so for the dynamic build,
 * libc.a's copy for the static build.
 */

#include "../bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*memcpy_fn)(void *, const void *, size_t);
static volatile memcpy_fn vmemcpy = memcpy;

struct mc_ctx {
    void *dst;
    const void *src;
    size_t n;
};

static void run_memcpy(uint64_t iters, void *vctx) {
    struct mc_ctx *c = vctx;
    void *dst = c->dst;
    const void *src = c->src;
    size_t n = c->n;
    for (uint64_t i = 0; i < iters; i++) {
        /* Read vmemcpy inside the loop — see strlen_sweep.c for the
         * full rationale.  Pulling the volatile read into a local
         * before the loop lets clang propagate the deduced value
         * (= libc memcpy) and apply memcpy's attributes, eliminating
         * the dead-store sequence of overwriting dst N-1 times. */
        vmemcpy(dst, src, n);
        bench_clobber();
    }
    bench_consume(dst);
}

static const size_t sweep_sizes[] = {
    1, 16, 64, 256, 1024, 4096, 16384, 65536,
};

void bench_libc_memcpy_sweep(void) {
    size_t max = sweep_sizes[sizeof(sweep_sizes) / sizeof(sweep_sizes[0]) - 1];
    char *src = malloc(max);
    char *dst = malloc(max);
    if (!src || !dst) { perror("malloc"); return; }
    /* Fill source with non-zero data so any conditional fast-paths in
     * libc (e.g. zero-skip) don't artificially inflate throughput. */
    for (size_t i = 0; i < max; i++) src[i] = (char)(i & 0xff);

    for (size_t i = 0; i < sizeof(sweep_sizes) / sizeof(sweep_sizes[0]); i++) {
        char cfg[32];
        snprintf(cfg, sizeof(cfg), "size=%zu", sweep_sizes[i]);
        struct mc_ctx ctx = { .dst = dst, .src = src, .n = sweep_sizes[i] };
        bench_time("libc", "memcpy", cfg, run_memcpy, &ctx);
    }

    free(src);
    free(dst);
}

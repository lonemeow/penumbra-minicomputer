/* libc/memset_sweep.c - memset(3) cost vs. buffer size.
 *
 * Same size sweep as memcpy.  Different access pattern: write-only, no
 * read traffic.  On a write-through D-cache (Penumbra), every store goes
 * to memory regardless of cache state, so this measures write bandwidth
 * end-to-end (CPU -> cache -> SDRAM bus).
 *
 * Comparing memset against memcpy at the same size: the difference is
 * the cost of reading the source.  If memset >> memcpy/2, the write path
 * is the bottleneck.  If memset ~= memcpy/2, reads and writes share the
 * bandwidth fairly.
 */

#include "../bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*memset_fn)(void *, int, size_t);
static volatile memset_fn vmemset = memset;

struct ms_ctx {
    void *buf;
    size_t n;
};

static void run_memset(uint64_t iters, void *vctx) {
    struct ms_ctx *c = vctx;
    void *buf = c->buf;
    size_t n = c->n;
    /* Vary the byte slightly each iteration so the optimizer can't
     * decide that all calls are identical and reorder/elide.  The
     * volatile pointer is also read inside the loop — see
     * strlen_sweep.c for the rationale. */
    for (uint64_t i = 0; i < iters; i++) {
        vmemset(buf, (int)(i & 0xff), n);
        bench_clobber();
    }
    bench_consume(buf);
}

static const size_t sweep_sizes[] = {
    1, 16, 64, 256, 1024, 4096, 16384, 65536,
};

void bench_libc_memset_sweep(void) {
    size_t max = sweep_sizes[sizeof(sweep_sizes) / sizeof(sweep_sizes[0]) - 1];
    char *buf = malloc(max);
    if (!buf) { perror("malloc"); return; }

    for (size_t i = 0; i < sizeof(sweep_sizes) / sizeof(sweep_sizes[0]); i++) {
        char cfg[32];
        snprintf(cfg, sizeof(cfg), "size=%zu", sweep_sizes[i]);
        struct ms_ctx ctx = { .buf = buf, .n = sweep_sizes[i] };
        bench_time("libc", "memset", cfg, run_memset, &ctx);
    }

    free(buf);
}

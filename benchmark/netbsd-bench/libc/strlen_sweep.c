/* libc/strlen_sweep.c - strlen(3) cost vs. string length.
 *
 * Sweeps short to medium lengths.  Hot path: every printf format scan,
 * every path-component traversal, every getenv/setenv, every dynamic
 * linker symbol lookup.
 *
 * The strings are heap-allocated and the length is hidden behind a
 * volatile function pointer so the compiler can't constant-fold strlen
 * on a string-literal argument.
 */

#include "../bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef size_t (*strlen_fn)(const char *);
static volatile strlen_fn vstrlen = strlen;

struct sl_ctx {
    const char *s;
};

static void run_strlen(uint64_t iters, void *vctx) {
    struct sl_ctx *c = vctx;
    const char *s = c->s;
    size_t total = 0;
    for (uint64_t i = 0; i < iters; i++) {
        /* The volatile load is *inside* the loop on purpose.  If we
         * stash vstrlen in a local before the loop, clang's value
         * propagation deduces "the only assignment to vstrlen is the
         * initializer, so the local equals strlen", attaches strlen's
         * __pure attribute, and hoists the call out of the loop.  Doing
         * the volatile load per iteration prevents that deduction. */
        total += vstrlen(s);
        bench_clobber();
    }
    bench_consume(&total);
}

static const size_t sweep_lens[] = {
    8, 64, 256, 1024, 4096,
};

void bench_libc_strlen_sweep(void) {
    size_t max = sweep_lens[sizeof(sweep_lens) / sizeof(sweep_lens[0]) - 1];
    char *buf = malloc(max + 1);
    if (!buf) { perror("malloc"); return; }
    /* Non-NUL printable bytes so any "skip past NUL"-style early-exits
     * in libc don't artificially shortcut the scan. */
    for (size_t i = 0; i < max; i++) buf[i] = (char)('a' + (i % 26));

    for (size_t i = 0; i < sizeof(sweep_lens) / sizeof(sweep_lens[0]); i++) {
        buf[sweep_lens[i]] = '\0';
        char cfg[32];
        snprintf(cfg, sizeof(cfg), "len=%zu", sweep_lens[i]);
        struct sl_ctx ctx = { .s = buf };
        bench_time("libc", "strlen", cfg, run_strlen, &ctx);
        /* Restore non-NUL so the next sweep point has the full string. */
        buf[sweep_lens[i]] = (char)('a' + (sweep_lens[i] % 26));
    }

    free(buf);
}

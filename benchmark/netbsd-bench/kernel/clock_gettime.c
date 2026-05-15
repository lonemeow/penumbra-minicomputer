/* kernel/clock_gettime.c - common-path syscall with kernel-side work.
 *
 * clock_gettime(CLOCK_MONOTONIC, ...) is a real workload signal: every
 * benchmark, every event-loop, every profiler hits it.  Unlike getpid,
 * the kernel does actual work (timecounter read).  The delta between
 * this and the getpid result is the timecounter cost.
 *
 * No VDSO on Penumbra, so this is a full syscall every time.  When/if
 * a VDSO arrives, this benchmark drops dramatically.
 */

#include "../bench.h"
#include <time.h>

static void run_clock_gettime(uint64_t iters, void *ctx) {
    (void)ctx;
    struct timespec ts;
    for (uint64_t i = 0; i < iters; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        bench_clobber();   /* Prevent clang from eliding all-but-last
                              write to ts; each call must materialize. */
    }
    bench_consume(&ts);
}

void bench_kernel_clock_gettime(void) {
    bench_time("kernel", "clock_gettime", NULL, run_clock_gettime, NULL);
}

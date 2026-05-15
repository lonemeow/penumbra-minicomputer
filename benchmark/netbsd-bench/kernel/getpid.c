/* kernel/getpid.c - minimum-syscall floor.
 *
 * getpid(2) reads a cached field in struct proc and returns; no locks,
 * no clock hardware, no copy.  This isolates the user->kernel->user
 * transition cost: trapframe save, syscall dispatch, return path.
 *
 * If you optimize the SYSCALL trap path, this number moves first.
 */

#include "../bench.h"
#include <unistd.h>
#include <sys/types.h>

static void run_getpid(uint64_t iters, void *ctx) {
    (void)ctx;
    pid_t p = 0;
    for (uint64_t i = 0; i < iters; i++) {
        p = getpid();
        bench_clobber();   /* getpid() is __pure in NetBSD libc; without
                              this clang hoists it out of the loop. */
    }
    bench_consume(&p);
}

void bench_kernel_getpid(void) {
    bench_time("kernel", "getpid", NULL, run_getpid, NULL);
}

/* kernel/fork_exit.c - process creation cost.
 *
 * fork(2) + immediate _exit(2) in the child + waitpid(2) in the parent.
 * Exercises: pmap_create (page-table allocation), COW setup for the
 * parent's address space, proc table allocation, kernel stack alloc,
 * scheduler insertion, exit teardown, and pmap_destroy.
 *
 * One of the heaviest "common" kernel paths.  Every shell pipeline,
 * every popen(3), every execve(2) target hits it.  Sensitive to pmap
 * efficiency — if pmap_copy or page-table cloning regresses, this is
 * where you see it first.
 */

#include "../bench.h"

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>

static void run_fork_exit(uint64_t iters, void *ctx) {
    (void)ctx;
    for (uint64_t i = 0; i < iters; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }
        if (pid == 0) {
            _exit(0);
        }
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return;
        }
    }
}

void bench_kernel_fork_exit(void) {
    bench_time("kernel", "fork_exit", NULL, run_fork_exit, NULL);
}

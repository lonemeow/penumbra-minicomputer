/* kernel/fork_exec.c - process spawn cost: fork(2) + execve(2) + exit + wait.
 *
 * The full "run a program" path: fork, the child replaces its image with
 * execve(2), runs to exit, and the parent reaps it.  This is what every
 * shell pipeline, popen(3), and system(3) actually does — fork_exit only
 * covers the fork/teardown half and never maps an executable page.
 *
 * Exercises, on top of fork_exit: execve image teardown + setup, demand-
 * paging the new program's text (plus ld.elf_so's and libc's, if dynamic),
 * and the per-executable-page I-cache synchronization in pmap_enter.
 * Re-execing the SAME target each iteration is the steady-state shell
 * workload: the text pages stay resident and I-cache-synced across
 * iterations, so this is the path that exposes the cost (or savings) of
 * how pmap handles executable mappings.
 */

#include "../bench.h"

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>

extern char **environ;

static void run_fork_exec(uint64_t iters, void *ctx) {
    (void)ctx;
    for (uint64_t i = 0; i < iters; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }
        if (pid == 0) {
            /*
             * Re-exec ourselves with the sentinel arg: no external target
             * to depend on, and the link mode matches automatically (the
             * static pbench execs static, the dynamic one execs dynamic).
             * main() sees the sentinel and _exit()s immediately.  execve
             * only returns on failure, so _exit(127) signals a bad path to
             * the parent's status check below.
             */
            char *cargv[] = {
                (char *)pbench_self_path,
                (char *)PBENCH_EXEC_CHILD_ARG,
                NULL
            };
            execve(pbench_self_path, cargv, environ);
            _exit(127);
        }
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) {
            fprintf(stderr,
                "fork_exec: child did not exec cleanly (status 0x%x) — "
                "check the execve target path\n", status);
            exit(1);   /* fail loud: a broken target must not read as a result */
        }
    }
}

void bench_kernel_fork_exec(void) {
    bench_time("kernel", "fork_exec", NULL, run_fork_exec, NULL);
}

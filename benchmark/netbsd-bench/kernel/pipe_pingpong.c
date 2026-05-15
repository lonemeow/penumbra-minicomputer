/* kernel/pipe_pingpong.c - context-switch cost via piped child.
 *
 * Forks a child.  Parent and child share two pipes (one each way) and
 * exchange a single byte back and forth `iters` times.  Each round-trip
 * involves: parent write -> kernel buffer -> child wakeup + reschedule
 * -> child read -> child write -> parent wakeup + reschedule -> parent
 * read.  Two context switches and four syscalls per iteration.
 *
 * Reports total ns per round-trip — divide by 2 for an upper bound on
 * a single context-switch.  Mixes IPC overhead in, but that's honest:
 * real-world context switches happen through IPC paths (pipes, sockets,
 * pthread sync, signals), not via sched_yield(2).
 */

#include "../bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

struct pipe_ctx {
    int p2c[2];   /* parent -> child */
    int c2p[2];   /* child -> parent */
    pid_t child;
};

static void child_loop(int read_fd, int write_fd) {
    /* Loops until parent closes its write end (EOF on read).  Echoes
     * each received byte back to the parent. */
    unsigned char buf;
    for (;;) {
        ssize_t r = read(read_fd, &buf, 1);
        if (r <= 0) _exit(0);
        if (write(write_fd, &buf, 1) != 1) _exit(1);
    }
}

static struct pipe_ctx *setup_pipes(void) {
    static struct pipe_ctx pc;
    if (pipe(pc.p2c) < 0 || pipe(pc.c2p) < 0) {
        perror("pipe");
        return NULL;
    }
    pc.child = fork();
    if (pc.child < 0) {
        perror("fork");
        return NULL;
    }
    if (pc.child == 0) {
        /* Child: close unused ends, run echo loop. */
        close(pc.p2c[1]);
        close(pc.c2p[0]);
        child_loop(pc.p2c[0], pc.c2p[1]);
        _exit(0);
    }
    /* Parent: close unused ends. */
    close(pc.p2c[0]);
    close(pc.c2p[1]);
    return &pc;
}

static void teardown_pipes(struct pipe_ctx *pc) {
    close(pc->p2c[1]);
    close(pc->c2p[0]);
    int status;
    waitpid(pc->child, &status, 0);
}

static void run_pingpong(uint64_t iters, void *ctx) {
    struct pipe_ctx *pc = ctx;
    unsigned char byte = 0;
    for (uint64_t i = 0; i < iters; i++) {
        if (write(pc->p2c[1], &byte, 1) != 1) { perror("write"); return; }
        if (read(pc->c2p[0], &byte, 1) != 1)  { perror("read");  return; }
    }
    bench_consume(&byte);
}

void bench_kernel_pipe_pingpong(void) {
    struct pipe_ctx *pc = setup_pipes();
    if (!pc) return;
    bench_time("kernel", "pipe_pingpong", "1B", run_pingpong, pc);
    teardown_pipes(pc);
}

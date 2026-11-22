/* pthread/pipe_pingpong.c - kernel/pipe_pingpong between two threads.
 *
 * The same one-byte pipe echo, four syscalls and two context switches a
 * round trip, with the partner a thread of this process instead of a
 * forked child.  Run in the same binary as kernel/pipe_pingpong, the two
 * differ only in whether each switch changes address space, so together
 * they price a process switch against a thread switch: the address-space
 * change and the TLB entries a second address space needs.
 *
 * Reports ns per round trip, as kernel/pipe_pingpong does.
 */

#include "../bench.h"
#include "partner.h"

#include <stdio.h>
#include <unistd.h>

struct pipe_pair {
    int to_partner[2];
    int to_main[2];
    pthread_t partner;
    uint64_t asked;      /* round trips the main thread completed */
    uint64_t answered;   /* bytes the partner read */
};

static void *pipe_partner(void *arg) {
    struct pipe_pair *pp = arg;
    unsigned char byte;

    /* Ends at EOF, once the main thread closes its write end. */
    while (read(pp->to_partner[0], &byte, 1) == 1) {
        pp->answered++;
        if (write(pp->to_main[1], &byte, 1) != 1) {
            perror("write");
            break;
        }
    }
    return NULL;
}

static int pipe_start(struct pipe_pair *pp) {
    pp->asked = pp->answered = 0;
    if (pipe(pp->to_partner) < 0)
        goto fail;
    if (pipe(pp->to_main) < 0) {
        close(pp->to_partner[0]);
        close(pp->to_partner[1]);
        goto fail;
    }
    if (partner_start(&pp->partner, pipe_partner, pp) < 0) {
        close(pp->to_partner[0]);
        close(pp->to_partner[1]);
        close(pp->to_main[0]);
        close(pp->to_main[1]);
        return -1;
    }
    return 0;
fail:
    perror("pipe");
    return -1;
}

static void pipe_stop(struct pipe_pair *pp) {
    close(pp->to_partner[1]);
    partner_join(pp->partner);
    close(pp->to_partner[0]);
    close(pp->to_main[0]);
    close(pp->to_main[1]);
    partner_check("pipe_pingpong", pp->asked, pp->answered);
}

static void run_pipe(uint64_t iters, void *ctx) {
    struct pipe_pair *pp = ctx;
    unsigned char byte = 0;
    uint64_t i;

    for (i = 0; i < iters; i++) {
        if (write(pp->to_partner[1], &byte, 1) != 1) { perror("write"); break; }
        if (read(pp->to_main[0], &byte, 1) != 1)     { perror("read");  break; }
    }
    pp->asked += i;
    bench_consume(&byte);
}

void bench_pthread_pipe_pingpong(void) {
    static struct pipe_pair pp;

    if (pipe_start(&pp) < 0)
        return;
    bench_time("pthread", "pipe_pingpong", "1B", run_pipe, &pp);
    pipe_stop(&pp);
}

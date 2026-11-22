/* pthread/registry.c - benchmarks that need libpthread.
 *
 * Only pbench-pthread links this, because linking libpthread makes the
 * whole process threaded (see suite.c).  Pingpong cases are named for
 * how the turn is handed over, so another mechanism is another case.
 */

#include "../bench.h"

extern void bench_pthread_pipe_pingpong(void);
extern void bench_pthread_cond_pingpong(void);
extern void bench_pthread_mutex(void);

static const struct bench_entry entries[] = {
    /* Thread switches, each comparable with kernel/pipe_pingpong. */
    { "pthread", "pipe_pingpong",  bench_pthread_pipe_pingpong },
    { "pthread", "cond_pingpong",  bench_pthread_cond_pingpong },

    /* Synchronisation without a switch. */
    { "pthread", "mutex",          bench_pthread_mutex },
};

const struct bench_table pbench_pthread = {
    entries, sizeof(entries) / sizeof(entries[0])
};

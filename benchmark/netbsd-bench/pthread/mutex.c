/* pthread/mutex.c - cost of an uncontended mutex lock and unlock.
 *
 * One pthread_mutex_lock and pthread_mutex_unlock pair on a mutex no
 * other thread touches.  That is the path every lock in a threaded
 * process takes when nobody holds it, libc's own included (stdio,
 * malloc), so it prices what being threaded adds to each of them.
 *
 * No second thread is needed for this to be the real path: libpthread
 * replaces libc's stub mutexes before main() runs, whether or not the
 * process ever creates a thread.
 *
 * Reports ns per lock-and-unlock pair.
 */

#include "../bench.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void run_mutex(uint64_t iters, void *ctx) {
    pthread_mutex_t *m = ctx;

    for (uint64_t i = 0; i < iters; i++) {
        pthread_mutex_lock(m);
        pthread_mutex_unlock(m);
    }
}

void bench_pthread_mutex(void) {
    static pthread_mutex_t m;
    int err;

    pthread_mutex_init(&m, NULL);
    /* Once, checked, so a mutex that only returns an error cannot read
     * as a fast one; the timed loop then runs the calls unchecked, as
     * code that holds a valid mutex does. */
    if ((err = pthread_mutex_lock(&m)) != 0 ||
        (err = pthread_mutex_unlock(&m)) != 0) {
        fprintf(stderr, "pthread/mutex: %s\n", strerror(err));
        pthread_mutex_destroy(&m);
        return;
    }
    bench_time("pthread", "mutex", "uncontended", run_mutex, &m);
    pthread_mutex_destroy(&m);
}

/* pthread/cond_pingpong.c - a turn passed between two threads.
 *
 * A mutex and condition variable hand a turn from the main thread to a
 * partner thread and back, which is how threads that share state wake
 * one another.  Each round trip blocks and wakes each side once: two
 * context switches, with the wakeups going through libpthread's park
 * and unpark rather than through a file as in pthread/pipe_pingpong.
 *
 * Reports ns per round trip.
 */

#include "../bench.h"
#include "partner.h"

#include <stdbool.h>

enum turn { TURN_MAIN, TURN_PARTNER };

struct cond_pair {
    pthread_mutex_t lock;
    pthread_cond_t cv;    /* signalled whenever turn changes */
    enum turn turn;       /* whose move it is */
    bool stop;            /* set with the turn that tells the partner to return */
    pthread_t partner;
    uint64_t asked;       /* round trips the main thread completed */
    uint64_t answered;    /* turns the partner handed back */
};

/* Gives the partner the turn and sleeps until it hands the turn back. */
static void hand_over(struct cond_pair *cp) {
    pthread_mutex_lock(&cp->lock);
    cp->turn = TURN_PARTNER;
    pthread_cond_signal(&cp->cv);
    while (cp->turn == TURN_PARTNER)
        pthread_cond_wait(&cp->cv, &cp->lock);
    pthread_mutex_unlock(&cp->lock);
}

static void *cond_partner(void *arg) {
    struct cond_pair *cp = arg;

    /* The turn is tested before every wait: the main thread usually
     * hands over the first turn before this thread has run at all, and
     * a signal sent with no waiter is not kept.  The stop turn is not an
     * answer, so it is recognised before counting. */
    pthread_mutex_lock(&cp->lock);
    for (;;) {
        while (cp->turn != TURN_PARTNER)
            pthread_cond_wait(&cp->cv, &cp->lock);
        if (cp->stop)
            break;
        cp->answered++;
        cp->turn = TURN_MAIN;
        pthread_cond_signal(&cp->cv);
    }
    pthread_mutex_unlock(&cp->lock);
    return NULL;
}

static int cond_start(struct cond_pair *cp) {
    pthread_mutex_init(&cp->lock, NULL);
    pthread_cond_init(&cp->cv, NULL);
    cp->turn = TURN_MAIN;
    cp->stop = false;
    cp->asked = cp->answered = 0;
    if (partner_start(&cp->partner, cond_partner, cp) < 0) {
        pthread_cond_destroy(&cp->cv);
        pthread_mutex_destroy(&cp->lock);
        return -1;
    }
    return 0;
}

static void cond_stop(struct cond_pair *cp) {
    pthread_mutex_lock(&cp->lock);
    cp->stop = true;
    cp->turn = TURN_PARTNER;
    pthread_cond_signal(&cp->cv);
    pthread_mutex_unlock(&cp->lock);
    partner_join(cp->partner);
    pthread_cond_destroy(&cp->cv);
    pthread_mutex_destroy(&cp->lock);
    partner_check("cond_pingpong", cp->asked, cp->answered);
}

static void run_cond(uint64_t iters, void *ctx) {
    struct cond_pair *cp = ctx;

    for (uint64_t i = 0; i < iters; i++)
        hand_over(cp);
    cp->asked += iters;
}

void bench_pthread_cond_pingpong(void) {
    static struct cond_pair cp;

    if (cond_start(&cp) < 0)
        return;
    bench_time("pthread", "cond_pingpong", NULL, run_cond, &cp);
    cond_stop(&cp);
}

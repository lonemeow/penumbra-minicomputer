/* pthread/partner.h - a second thread for the pingpong cases.
 *
 * Each pingpong case starts a partner thread that answers the main
 * thread's turns, joins it afterwards, and checks the partner answered
 * exactly as many turns as it was handed.  The check is what keeps a
 * broken handoff from reading as a result.
 */

#ifndef PBENCH_PTHREAD_PARTNER_H
#define PBENCH_PTHREAD_PARTNER_H

#include <pthread.h>
#include <stdint.h>

/* 0 when the partner is running; -1, reported, when it could not start. */
int partner_start(pthread_t *t, void *(*fn)(void *), void *arg);

/* Waits for the partner to return; a failed join ends the run. */
void partner_join(pthread_t t);

/* Ends the run unless the partner answered every turn it was asked.
 * Call after partner_join, which is what makes the partner's count
 * visible to this thread. */
void partner_check(const char *name, uint64_t asked, uint64_t answered);

#endif /* PBENCH_PTHREAD_PARTNER_H */

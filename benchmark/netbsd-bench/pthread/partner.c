/* pthread/partner.c - starting, joining and checking a partner thread. */

#include "partner.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int partner_start(pthread_t *t, void *(*fn)(void *), void *arg) {
    int err = pthread_create(t, NULL, fn, arg);
    if (err != 0) {
        /* pthread functions return the error rather than setting errno. */
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        return -1;
    }
    return 0;
}

void partner_join(pthread_t t) {
    int err = pthread_join(t, NULL);
    if (err != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(err));
        exit(1);
    }
}

void partner_check(const char *name, uint64_t asked, uint64_t answered) {
    if (answered != asked) {
        fprintf(stderr,
            "pthread/%s: partner answered %" PRIu64 " of %" PRIu64
            " round trips\n", name, answered, asked);
        exit(1);   /* fail loud: a broken handoff must not read as a result */
    }
}

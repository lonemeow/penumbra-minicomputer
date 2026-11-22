/* suite.c - what pbench carries: the base categories, without libpthread.
 *
 * Linking libpthread would make the whole process threaded from startup,
 * changing what every other case measures, so the cases that need it
 * live in pbench-pthread (suite_pthread.c).
 */

#include "bench.h"

#include <stddef.h>

const struct bench_table *const pbench_suite[] = {
    &pbench_base,
    NULL,
};

const char pbench_suite_name[] = "pbench";

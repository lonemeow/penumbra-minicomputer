/* suite_pthread.c - what pbench-pthread carries: everything pbench does,
 * then the pthread category.
 *
 * The base cases are the same objects pbench links, so running one in
 * both binaries prices what being a threaded process costs it.
 */

#include "bench.h"

#include <stddef.h>

const struct bench_table *const pbench_suite[] = {
    &pbench_base,
    &pbench_pthread,
    NULL,
};

const char pbench_suite_name[] = "pbench-pthread";

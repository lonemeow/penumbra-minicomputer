/*
 * port.h — what the CoreMark port offers the harness
 *
 * CoreMark's main() is one pass: it initializes its data from three
 * seeds, runs a number of iterations inside a timed region, checks the
 * results against the CRCs it knows for those seeds and prints a
 * report.  The harness runs it as many times as it needs, choosing the
 * seeds and the iteration count each time.
 */

#ifndef COREMARK_PORT_H
#define COREMARK_PORT_H

#include "bench.h"

/* The seed sets the run rules name; main() recognizes each by a CRC of
 * its seeds and holds the expected results for both. */
typedef struct {
    int seed1, seed2, seed3;
} coremark_seeds_t;

#define COREMARK_SEEDS_PERFORMANCE ((coremark_seeds_t){ 0x0,    0x0,    0x66 })
#define COREMARK_SEEDS_VALIDATION  ((coremark_seeds_t){ 0x3415, 0x3415, 0x66 })

/* What one pass measured.  The counters bracket exactly the timed
 * region, the one CoreMark's own figures cover. */
typedef struct {
    uint32_t           iterations;  /* as main() ran them                */
    uint32_t           ticks;       /* timer ticks in the timed region   */
    int                errors;      /* results that failed their CRC     */
    bench_perf_t       cpu_before, cpu_after;
    bench_cache_perf_t l1d_before, l1d_after;
    bench_cache_perf_t l1i_before, l1i_after;
    bench_cache_perf_t l2_before,  l2_after;
} coremark_pass_t;

/*
 * Run one pass.  `iterations` must not be 0, which main() takes as a
 * request to choose the count itself.  With `quiet` set CoreMark's
 * report is discarded, for a pass run only to be timed.
 */
void coremark_run_pass(coremark_seeds_t seeds, uint32_t iterations,
                       int quiet, coremark_pass_t *out);

#endif /* COREMARK_PORT_H */

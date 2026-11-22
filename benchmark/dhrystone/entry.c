/*
 * entry.c — bench_main wrapper for Dhrystone
 *
 * Dhrystone's main() is renamed to dhrystone_main() via
 * -Dmain=dhrystone_main on the compiler command line.
 * This file provides the actual bench_main entry point and
 * prints timing from the hardware timer (Dhrystone's own float
 * output relies on soft-float stubs, so it prints 0.0), over the
 * window Dhrystone times (window.h).
 */

#include "bench.h"
#include "window.h"

#ifndef DHRYSTONE_ITERATIONS
#define DHRYSTONE_ITERATIONS 100
#endif

/* VAX 11/780 reference score: 1 DMIPS = 1757 Dhrystones/sec */
#define VAX_DMIPS_REF 1757

extern int dhrystone_main(void);

static bench_perf_t perf_before, perf_after;
static bench_cache_perf_t l1d_before, l1d_after;
static bench_cache_perf_t l1i_before, l1i_after;
static bench_cache_perf_t l2_before,  l2_after;
static uint32_t window_ticks;
static int window_closed;

/* The counters are read inside the timer's reading, so the timer covers
 * every cycle they count. */
void dhry_window_open(void) {
    bench_timer_start();
    bench_perf_snapshot(&perf_before);
    bench_cache_perf_snapshot_l1d(&l1d_before);
    bench_cache_perf_snapshot_l1i(&l1i_before);
    bench_cache_perf_snapshot_l2 (&l2_before);
}

uint32_t dhry_window_close(void) {
    bench_cache_perf_snapshot_l1d(&l1d_after);
    bench_cache_perf_snapshot_l1i(&l1i_after);
    bench_cache_perf_snapshot_l2 (&l2_after);
    bench_perf_snapshot(&perf_after);
    window_ticks = bench_timer_elapsed_ticks();
    window_closed = 1;
    return window_ticks;
}

void bench_main(uint32_t bootdata) {
    (void)bootdata;
    bench_init();

    dhrystone_main();

    if (!window_closed) {
        bench_puts("\n*** Dhrystone never closed its timed window ***\n");
        return;
    }

    uint32_t ticks = window_ticks;
    uint32_t freq = bench_timer_freq_hz();
    uint32_t us = (uint32_t)((uint64_t)ticks * 1000000u / freq);

    bench_puts("\n--- Harness timing ---\n");
    bench_puts("Timer frequency: ");
    bench_print_uint(freq);
    bench_puts(" Hz\n");
    bench_puts("Elapsed ticks:   ");
    bench_print_uint(ticks);
    bench_puts("\n");
    bench_puts("Elapsed us:      ");
    bench_print_uint(us);
    bench_puts("\n");
    bench_puts("Iterations:      ");
    bench_print_uint(DHRYSTONE_ITERATIONS);
    bench_puts("\n");
    if (ticks > 0) {
        /* In thousandths, each straight from the tick count. */
        uint64_t us_per_iter_milli = (uint64_t)ticks * 1000000u * 1000u
            / ((uint64_t)freq * DHRYSTONE_ITERATIONS);
        uint64_t dhrystones_per_sec_milli =
            (uint64_t)DHRYSTONE_ITERATIONS * 1000u * freq / ticks;
        uint64_t dmips_milli = dhrystones_per_sec_milli / VAX_DMIPS_REF;

        bench_puts("us/iteration:    ");
        bench_print_milli(us_per_iter_milli);
        bench_puts("\n");
        bench_puts("Dhrystones/sec:  ");
        bench_print_milli(dhrystones_per_sec_milli);
        bench_puts("\n");
        bench_puts("DMIPS:           ");
        bench_print_milli(dmips_milli);
        bench_puts("\n");
    }

    bench_perf_print_delta("CPU perfctrs", &perf_before, &perf_after);

    bench_puts("\n--- Cache perfctrs ---\n");
    bench_cache_perf_print_delta("L1-D", &l1d_before, &l1d_after);
    bench_cache_perf_print_delta("L1-I", &l1i_before, &l1i_after);
    bench_cache_perf_print_delta("L2  ", &l2_before,  &l2_after);
}

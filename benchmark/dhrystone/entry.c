/*
 * entry.c — bench_main wrapper for Dhrystone
 *
 * Dhrystone's main() is renamed to dhrystone_main() via
 * -Dmain=dhrystone_main on the compiler command line.
 * This file provides the actual bench_main entry point and
 * prints timing from the hardware timer (Dhrystone's own float
 * output relies on soft-float stubs, so it prints 0.0).
 */

#include "bench.h"

#ifndef DHRYSTONE_ITERATIONS
#define DHRYSTONE_ITERATIONS 100
#endif

/* VAX 11/780 reference score: 1 DMIPS = 1757 Dhrystones/sec */
#define VAX_DMIPS_REF 1757

extern int dhrystone_main(void);

void bench_main(uint32_t bootdata) {
    (void)bootdata;
    bench_init();

    bench_perf_t perf_before, perf_after;
    bench_cache_perf_t l1d_before, l1d_after;
    bench_cache_perf_t l1i_before, l1i_after;
    bench_cache_perf_t l2_before,  l2_after;

    bench_timer_start();
    bench_perf_snapshot(&perf_before);
    bench_cache_perf_snapshot_l1d(&l1d_before);
    bench_cache_perf_snapshot_l1i(&l1i_before);
    bench_cache_perf_snapshot_l2 (&l2_before);
    dhrystone_main();
    bench_cache_perf_snapshot_l1d(&l1d_after);
    bench_cache_perf_snapshot_l1i(&l1i_after);
    bench_cache_perf_snapshot_l2 (&l2_after);
    bench_perf_snapshot(&perf_after);

    /* Print real timing from hardware timer */
    uint32_t ticks = bench_timer_elapsed_ticks();
    uint32_t us = bench_timer_elapsed_us();
    uint32_t freq = bench_timer_freq_hz();

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
    if (us > 0) {
        uint32_t us_per_iter = us / DHRYSTONE_ITERATIONS;
        uint32_t dhrystones_per_sec = us_per_iter > 0
            ? 1000000 / us_per_iter : 0;
        uint32_t dmips_x100 = dhrystones_per_sec * 100 / VAX_DMIPS_REF;

        bench_puts("us/iteration:    ");
        bench_print_uint(us_per_iter);
        bench_puts("\n");
        bench_puts("Dhrystones/sec:  ");
        bench_print_uint(dhrystones_per_sec);
        bench_puts("\n");
        bench_puts("DMIPS:           ");
        bench_print_uint(dmips_x100 / 100);
        bench_putchar('.');
        uint32_t frac = dmips_x100 % 100;
        if (frac < 10)
            bench_putchar('0');
        bench_print_uint(frac);
        bench_puts("\n");
    }

    bench_perf_print_delta("CPU perfctrs", &perf_before, &perf_after);

    bench_puts("\n--- Cache perfctrs ---\n");
    bench_cache_perf_print_delta("L1-D", &l1d_before, &l1d_after);
    bench_cache_perf_print_delta("L1-I", &l1i_before, &l1i_after);
    bench_cache_perf_print_delta("L2  ", &l2_before,  &l2_after);
}

/*
Copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Original Author: Shay Gal-on

This file has been changed from EEMBC's barebones/core_portme.c: it is
the Penumbra bare-metal port, on the benchmark harness's timer, console
and performance counters.
*/
#include "coremark.h"
#include "port.h"

int coremark_main(void);

/* main() reads these once per pass, after portable_init(). */
volatile ee_s32 seed1_volatile;
volatile ee_s32 seed2_volatile;
volatile ee_s32 seed3_volatile;
volatile ee_s32 seed4_volatile;     /* iterations                        */
volatile ee_s32 seed5_volatile;     /* algorithms to run; 0 selects all  */

ee_u32 default_num_contexts = 1;

/* The pass in progress. */
static coremark_pass_t pass;
static int             console_quiet;

static void
fatal(const char *msg)
{
    bench_puts("coremark port: ");
    bench_puts(msg);
    bench_puts("\n");
    __asm__ __volatile__("break");
}

/* The timed region is start_time() to stop_time(), so the timer and
 * the CPU counters are the innermost pair and the cache counters, which
 * take longer to read, sit outside them. */
void
start_time(void)
{
    bench_cache_perf_snapshot_l1d(&pass.l1d_before);
    bench_cache_perf_snapshot_l1i(&pass.l1i_before);
    bench_cache_perf_snapshot_l2(&pass.l2_before);
    bench_timer_start();
    bench_perf_snapshot(&pass.cpu_before);
}

void
stop_time(void)
{
    bench_perf_snapshot(&pass.cpu_after);
    pass.ticks = bench_timer_elapsed_ticks();
    bench_cache_perf_snapshot_l1d(&pass.l1d_after);
    bench_cache_perf_snapshot_l1i(&pass.l1i_after);
    bench_cache_perf_snapshot_l2(&pass.l2_after);
}

CORE_TICKS
get_time(void)
{
    return pass.ticks;
}

/* Whole seconds, HAS_FLOAT being 0.  main() only uses the result to
 * print a truncated score and to enforce the ten-second minimum. */
secs_ret
time_in_secs(CORE_TICKS ticks)
{
    return ticks / bench_timer_freq_hz();
}

void
portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
    {
        ee_printf(
            "ERROR! Please define ee_ptr_int to a type that holds a "
            "pointer!\n");
    }
    if (sizeof(ee_u32) != 4)
    {
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    }
    p->portable_id = 1;
}

/* main() keeps its results to itself, but hands over the port's member
 * of them, and by now it has run the iterations and counted the CRC
 * mismatches.  The harness needs both: the count to compute a score,
 * the mismatches to refuse one. */
void
portable_fini(core_portable *p)
{
    const core_results *res
        = (const core_results *)((const char *)p
                                 - offsetof(core_results, port));

    pass.iterations = res->iterations;
    pass.errors     = res->err;
    p->portable_id  = 0;
}

void
coremark_console_putchar(char c)
{
    if (console_quiet)
        return;
    if (c == '\n')
        bench_putchar('\r');
    bench_putchar(c);
}

void
coremark_run_pass(coremark_seeds_t seeds, uint32_t iterations, int quiet,
                  coremark_pass_t *out)
{
    if (iterations == 0)
        fatal("a pass needs an iteration count");

    seed1_volatile = seeds.seed1;
    seed2_volatile = seeds.seed2;
    seed3_volatile = seeds.seed3;
    seed4_volatile = (ee_s32)iterations;
    seed5_volatile = 0;

    console_quiet = quiet;
    coremark_main();
    console_quiet = 0;

    if (pass.iterations != iterations)
        fatal("CoreMark ran a different number of iterations than asked");

    *out = pass;
}

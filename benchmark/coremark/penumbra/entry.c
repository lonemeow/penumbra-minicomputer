/*
 * entry.c — bench_main for CoreMark
 *
 * Runs CoreMark the way its run rules ask for a reportable score: the
 * validation seeds and the performance seeds, each for at least ten
 * seconds, with every result checked against the CRC CoreMark knows
 * for it.  CoreMark prints its own report for each; the score comes
 * from the performance run.
 *
 * How many iterations ten seconds takes depends on the machine, so
 * each seed set is run twice: a short pass with its report discarded,
 * which times an iteration, then the measured pass sized from it.
 *
 * CoreMark keeps whole seconds here (HAS_FLOAT is 0) and its
 * Iterations/Sec is truncated to match, so the scores below are the
 * harness's own, from the timer and the CPU cycle counter.
 */

#include "bench.h"
#include "core_portme.h"
#include "port.h"

/* The run rules' minimum for a result that may be reported. */
#define MIN_RUN_SECONDS         10

/* Iterations in the pass that times one.  The first iteration runs on
 * cold caches, so one alone would overstate what an iteration takes. */
#define CALIBRATION_ITERATIONS  10

/*
 * Choose the iteration count for a measured pass.
 *
 * The calibration pass ran `cal_iterations` iterations of the same
 * seed set in `cal_ticks` timer ticks, the timer counting
 * `ticks_per_sec` ticks a second.  Returns the count to run, or 0 if
 * the calibration gives nothing to size a pass from.
 *
 * The measured pass has to last MIN_RUN_SECONDS or CoreMark rejects it.
 */
static uint32_t measured_iterations(uint32_t cal_iterations,
                                    uint32_t cal_ticks,
                                    uint32_t ticks_per_sec) {
    uint32_t cal_ticks_per_iter = cal_ticks / cal_iterations + 1;
    uint64_t min_iters =
        (uint64_t)ticks_per_sec * (MIN_RUN_SECONDS + 1) / cal_ticks_per_iter
        + 1;

    if (min_iters <= UINT32_MAX)
        return (uint32_t)min_iters;
    else
        return 0;
}

/* A pass counts toward a score if every result matched its CRC and it
 * ran for the minimum time. */
static int pass_is_valid(const coremark_pass_t *p) {
    return p->errors == 0
        && p->ticks / bench_timer_freq_hz() >= MIN_RUN_SECONDS;
}

static void print_pass_verdict(const char *label, const coremark_pass_t *p) {
    bench_puts(label);
    if (p->errors != 0) {
        bench_puts("FAILED, ");
        bench_print_int(p->errors);
        bench_puts(" CRC mismatches\n");
    } else if (!pass_is_valid(p)) {
        bench_puts("too short, under ");
        bench_print_uint(MIN_RUN_SECONDS);
        bench_puts(" s\n");
    } else {
        bench_puts("validated\n");
    }
}

/* Calibrate, then run the measured pass into `out`.  Returns 0 when no
 * measured pass ran. */
static int run_seed_set(const char *name, coremark_seeds_t seeds,
                        coremark_pass_t *out) {
    coremark_pass_t cal;

    bench_puts("\n=== CoreMark, ");
    bench_puts(name);
    bench_puts(" ===\n");

    coremark_run_pass(seeds, CALIBRATION_ITERATIONS, 1, &cal);

    uint32_t iterations = measured_iterations(CALIBRATION_ITERATIONS,
                                              cal.ticks,
                                              bench_timer_freq_hz());
    if (iterations == 0) {
        bench_puts("*** no usable iteration count from a calibration pass of ");
        bench_print_uint(cal.ticks);
        bench_puts(" ticks\n");
        return 0;
    }

    coremark_run_pass(seeds, iterations, 0, out);
    return 1;
}

static void print_compiler_and_location(void) {
    bench_puts(" / " COMPILER_VERSION " " COMPILER_FLAGS " / " MEM_LOCATION);
}

/*
 * The lines CoreMark's reporting rules define, for quoting as they
 * stand.  `score_milli` is iterations a second and `per_mhz_milli`
 * iterations per million CPU cycles, both in thousandths.
 *
 * The per-MHz line has to state the memory and how its clock stands to
 * the CPU's.  Both come from the machine, so a machine the harness
 * cannot describe gets no such line, and a simulated one gets neither:
 * its timings are not measurements.
 */
static void print_reporting_lines(uint64_t score_milli,
                                  uint64_t per_mhz_milli) {
    const bench_memory_t *mem = bench_machine_memory();
    uint32_t cpu_hz = bench_cpu_freq_hz();

    bench_puts("\n--- Reporting lines ---\n");
    if (mem && mem->simulated) {
        bench_puts("None: on a simulated machine the figures above count "
                   "instructions, not time.\n");
        return;
    }

    bench_puts("CoreMark 1.0 : ");
    bench_print_milli(score_milli);
    print_compiler_and_location();
    bench_puts("\n");

    if (!mem) {
        char machine[BENCH_NAME_LEN];

        bench_machine_name(machine);
        bench_puts("*** No CoreMark/MHz line: it has to state the memory, "
                   "and machine \"");
        bench_puts(machine);
        bench_puts("\"\n*** is not in machine_memory[] "
                   "(benchmark/common/bench.c).\n");
        return;
    }
    if (cpu_hz == 0 || per_mhz_milli == 0) {
        bench_puts("*** No CoreMark/MHz line: the machine reports no CPU "
                   "clock or no cycle count.\n");
        return;
    }

    bench_puts("CoreMark/MHz 1.0 : ");
    bench_print_milli(per_mhz_milli);
    print_compiler_and_location();
    bench_puts(", ");
    bench_puts(mem->memory);
    bench_puts(", ");
    bench_print_milli((uint64_t)mem->clock_hz * 1000u / cpu_hz);
    bench_puts(":1 Memory, 1:1 Cache\n");
}

void bench_main(uint32_t bootdata) {
    (void)bootdata;
    bench_init();

    coremark_pass_t validation, performance;

    int ran = run_seed_set("validation run", COREMARK_SEEDS_VALIDATION,
                           &validation)
           && run_seed_set("performance run", COREMARK_SEEDS_PERFORMANCE,
                           &performance);

    bench_puts("\n--- Harness report ---\n");
    if (!ran) {
        bench_puts("*** No score: a run did not take place.\n");
        return;
    }

    print_pass_verdict("Validation run:  ", &validation);
    print_pass_verdict("Performance run: ", &performance);
    if (!pass_is_valid(&validation) || !pass_is_valid(&performance)) {
        bench_puts("*** No score: both runs have to validate.\n");
        return;
    }

    const coremark_pass_t *p = &performance;
    uint32_t freq   = bench_timer_freq_hz();
    uint32_t cycles = p->cpu_after.cycles - p->cpu_before.cycles;

    /* Scores in thousandths.  Iterations per million CPU cycles is the
     * score per MHz of CPU clock, with no clock frequency involved. */
    uint64_t score_milli   = (uint64_t)p->iterations * 1000u * freq / p->ticks;
    uint64_t per_mhz_milli = cycles == 0 ? 0
        : (uint64_t)p->iterations * 1000u * 1000000u / cycles;

    char machine[BENCH_NAME_LEN], cpu[BENCH_NAME_LEN];

    bench_machine_name(machine);
    bench_cpu_name(cpu);
    bench_puts("Machine:         ");
    bench_puts(machine);
    bench_puts(", ");
    bench_puts(cpu);
    bench_puts(" at ");
    bench_print_milli(bench_cpu_freq_hz() / 1000u);
    bench_puts(" MHz\nIterations:      ");
    bench_print_uint(p->iterations);
    bench_puts("\nElapsed:         ");
    bench_print_uint(p->ticks);
    bench_puts(" ticks at ");
    bench_print_uint(freq);
    bench_puts(" Hz\nIterations/sec:  ");
    bench_print_milli(score_milli);
    bench_puts("\nPer MHz:         ");
    bench_print_milli(per_mhz_milli);
    bench_puts("\n");

    bench_perf_print_delta("CPU perfctrs", &p->cpu_before, &p->cpu_after);

    bench_puts("\n--- Cache perfctrs ---\n");
    bench_cache_perf_print_delta("L1-D", &p->l1d_before, &p->l1d_after);
    bench_cache_perf_print_delta("L1-I", &p->l1i_before, &p->l1i_after);
    bench_cache_perf_print_delta("L2  ", &p->l2_before,  &p->l2_after);

    print_reporting_lines(score_milli, per_mhz_milli);
}

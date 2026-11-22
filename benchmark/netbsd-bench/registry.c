/* registry.c - the benchmarks every pbench binary carries.
 *
 * Add a benchmark by dropping its .c file in the right directory,
 * declaring its entry function here, and appending an entry.  A
 * benchmark that needs a library only some binaries link belongs in
 * that library's own registry instead (pthread/registry.c).
 */

#include "bench.h"

extern void bench_kernel_getpid(void);
extern void bench_kernel_clock_gettime(void);
extern void bench_kernel_pipe_pingpong(void);
extern void bench_kernel_fork_exit(void);
extern void bench_kernel_fork_exec(void);

extern void bench_libc_memcpy_sweep(void);
extern void bench_libc_memcpy_align(void);
extern void bench_libc_memset_sweep(void);
extern void bench_libc_strlen_sweep(void);
extern void bench_libc_qsort_int(void);

extern void bench_audio_mix(void);

static const struct bench_entry entries[] = {
    /* Kernel benchmarks: cross-cutting cost signals. */
    { "kernel", "getpid",         bench_kernel_getpid },
    { "kernel", "clock_gettime",  bench_kernel_clock_gettime },
    { "kernel", "pipe_pingpong",  bench_kernel_pipe_pingpong },
    { "kernel", "fork_exit",      bench_kernel_fork_exit },
    { "kernel", "fork_exec",      bench_kernel_fork_exec },

    /* libc benchmarks: hot routines, size-swept where applicable. */
    { "libc",   "memcpy",         bench_libc_memcpy_sweep },
    { "libc",   "memcpy_align",   bench_libc_memcpy_align },
    { "libc",   "memset",         bench_libc_memset_sweep },
    { "libc",   "strlen",         bench_libc_strlen_sweep },
    { "libc",   "qsort_int",      bench_libc_qsort_int },

    /* Audio benchmarks: what playing music costs the CPU. */
    { "audio",  "mix",            bench_audio_mix },
};

const struct bench_table pbench_base = {
    entries, sizeof(entries) / sizeof(entries[0])
};

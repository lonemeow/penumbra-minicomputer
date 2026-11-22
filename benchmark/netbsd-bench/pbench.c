/* pbench.c - Penumbra NetBSD-hosted benchmark suite.
 *
 * Usage:
 *   pbench [-o FILE]                       Run every benchmark.
 *   pbench [-o FILE] <category>            Run all benchmarks in a category.
 *   pbench [-o FILE] <category> <name>     Run one benchmark.
 *   pbench list                            List all registered benchmarks.
 *   pbench help                            Show help.
 *
 * Options:
 *   -o FILE   Write machine-readable RESULT lines to FILE.  Without this
 *             flag, only the human-readable table is printed to stdout —
 *             convenient for pasting into a writeup.
 *   -p        Also dump CPU + cache performance counters (stall breakdown,
 *             cache hit rates) under each benchmark.  Off by default; adds
 *             one extra measured batch per benchmark.
 *
 * Every run starts with a "# <binary> on <CPU> @ <clock>" header so saved
 * outputs are self-describing when comparing files.
 *
 * Categories:
 *   kernel    Syscall / context-switch / process-creation costs.
 *   libc      memcpy / memset / strlen / qsort etc., size-swept.
 *   audio     Software mixing of tracker voices.
 *   pthread   Thread switches and mutexes (pbench-pthread only).
 *
 * pbench-pthread runs the same objects as pbench, linked with libpthread.
 * libpthread starts before main() and makes the whole process threaded:
 * libc takes its locks for real and fork runs its handlers.  A case run in
 * both binaries therefore prices what linking libpthread costs it.
 */

#include "bench.h"
#include "perfctr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

/* Path to this executable, used by self-exec spawn benchmarks
 * (see bench.h / kernel/fork_exec.c).  Set once at the top of main(). */
const char *pbench_self_path = NULL;

/* Resolve an absolute, execve()-able path to ourselves.  argv[0] works
 * when we were invoked with a path; when invoked via PATH (no slash) it
 * is just "pbench", which execve() cannot use, so fall back to asking the
 * kernel for our pathname. */
static const char *resolve_self_path(const char *argv0) {
    if (argv0 != NULL && strchr(argv0, '/') != NULL)
        return argv0;
    static char buf[1024];
    int mib[4] = { CTL_KERN, KERN_PROC_ARGS, getpid(), KERN_PROC_PATHNAME };
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, NULL, 0) == 0 && len > 0)
        return buf;
    return argv0;  /* best effort; fork_exec's child-status check catches a bad path */
}

/* --- CLI helpers ---------------------------------------------------- */

static void print_help(const char *argv0) {
    fprintf(stderr,
        "usage: %s [-o FILE] [help|list|<category> [<name>]]\n"
        "  no args                  run every benchmark\n"
        "  <category>               run all benchmarks in category (",
        argv0);
    /* Each registry holds one or more whole categories, in order. */
    const char *last_cat = NULL;
    for (const struct bench_table *const *t = pbench_suite; *t; t++)
        for (size_t i = 0; i < (*t)->len; i++) {
            const char *cat = (*t)->entries[i].category;
            if (last_cat && strcmp(cat, last_cat) == 0) continue;
            fprintf(stderr, "%s%s", last_cat ? "|" : "", cat);
            last_cat = cat;
        }
    fprintf(stderr, ")\n"
        "  <category> <name>        run one benchmark\n"
        "  list                     list registered benchmarks\n"
        "  help                     show this help\n"
        "options:\n"
        "  -o FILE                  also write machine-readable RESULT lines to FILE\n"
        "  -p                       dump CPU + cache perfctrs per benchmark (slower)\n");
}

/* Emit a one-line header naming the binary, the CPU model and the clock, so
 * a saved run is self-describing when comparing result files: pbench and
 * pbench-pthread report the same cases differently.  Reads the kernel's
 * hw.model and machdep.cpu.freq; degrades gracefully if either is missing. */
static void print_system_header(FILE *out) {
    char model[64];
    size_t len = sizeof(model);
    /* machdep.cpu.model is the CPU identity ("Penumbra/2"); hw.model names
     * the board.  The CPU is what matters for performance comparison. */
    if (sysctlbyname("machdep.cpu.model", model, &len, NULL, 0) != 0)
        strlcpy(model, "unknown CPU", sizeof(model));

    uint64_t freq = 0;
    len = sizeof(freq);
    (void)sysctlbyname("machdep.cpu.freq", &freq, &len, NULL, 0);

    fprintf(out, "# %s on %s", pbench_suite_name, model);
    if (freq)
        fprintf(out, " @ %llu.%03llu MHz",
                (unsigned long long)(freq / 1000000ull),
                (unsigned long long)((freq % 1000000ull) / 1000ull));
    fprintf(out, "\n");
    fflush(out);
}

static void list_benchmarks(void) {
    printf("Registered benchmarks:\n");
    const char *last_cat = "";
    for (const struct bench_table *const *t = pbench_suite; *t; t++)
        for (size_t i = 0; i < (*t)->len; i++) {
            const struct bench_entry *e = &(*t)->entries[i];
            if (strcmp(e->category, last_cat) != 0) {
                printf("\n  %s:\n", e->category);
                last_cat = e->category;
            }
            printf("    %s\n", e->name);
        }
}

static void run_one(const struct bench_entry *e) {
    printf("\n--- %s/%s ---\n", e->category, e->name);
    fflush(stdout);
    e->run();
}

static int run_filtered(const char *category, const char *name) {
    int matched = 0;
    for (const struct bench_table *const *t = pbench_suite; *t; t++)
        for (size_t i = 0; i < (*t)->len; i++) {
            const struct bench_entry *e = &(*t)->entries[i];
            if (category && strcmp(category, e->category) != 0) continue;
            if (name     && strcmp(name,     e->name)     != 0) continue;
            run_one(e);
            matched++;
        }
    if (matched == 0) {
        fprintf(stderr, "%s: no benchmark matches", pbench_suite_name);
        if (category) fprintf(stderr, " category=%s", category);
        if (name)     fprintf(stderr, " name=%s", name);
        fprintf(stderr, "\n");
        return 1;
    }
    return 0;
}

/* --- Main ----------------------------------------------------------- */

int main(int argc, char **argv) {
    /* Self-exec spawn target: fork_exec re-execs us with this sentinel.
     * Exit immediately and as early as possible so the measured cost is
     * the spawn (fork+execve+runtime startup+exit), not benchmark work. */
    if (argc >= 2 && strcmp(argv[1], PBENCH_EXEC_CHILD_ARG) == 0)
        _exit(0);

    const char *argv0 = argv[0];
    pbench_self_path = resolve_self_path(argv0);
    FILE *result_fp = NULL;

    /* Parse leading option flags (-o FILE, -p) before positional args. */
    while (argc >= 2 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-o") == 0 && argc >= 3) {
            result_fp = fopen(argv[2], "w");
            if (!result_fp) {
                fprintf(stderr, "pbench: cannot open %s for writing: %s\n",
                        argv[2], strerror(errno));
                return 2;
            }
            bench_set_result_file(result_fp);
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[1], "-p") == 0) {
            bench_perfctr_enabled = 1;
            argv += 1;
            argc -= 1;
        } else {
            break;  /* -h / --help / unknown — handled in positional parsing */
        }
        argv[0] = (char *)argv0;  /* preserve program name for help */
    }

    int rc;
    if (argc >= 2 && (strcmp(argv[1], "help") == 0 ||
                      strcmp(argv[1], "-h")   == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        print_help(argv[0]);
        rc = 0;
    } else if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        list_benchmarks();
        rc = 0;
    } else {
        /* A benchmark run: emit the system header first (stdout, and the
         * result file if one was opened) so outputs are self-describing. */
        print_system_header(stdout);
        if (result_fp) print_system_header(result_fp);

        if (argc == 1)      rc = run_filtered(NULL, NULL);
        else if (argc == 2) rc = run_filtered(argv[1], NULL);
        else if (argc == 3) rc = run_filtered(argv[1], argv[2]);
        else { print_help(argv[0]); rc = 2; }
    }

    if (result_fp) fclose(result_fp);
    return rc;
}

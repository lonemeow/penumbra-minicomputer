// Penumbra's hardware counters, summarized every few seconds under -perfctr.
//
// RDSYS is privileged, so the counters come through the kernel's bulk
// machdep.cpu and machdep.cache sysctls.  Each summary covers the frames
// since the one before, so a run shows how the cost moves with the scene
// rather than one average over the whole of it.

#include <sys/sysctl.h>

#include <machine/sysreg.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "doomtype.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"

// Each delta is one subtraction of free-running 32-bit counters, so a
// window has to stay under 2^32 cycles: ~115 s at 37.5 MHz.
#define PERFCTR_PERIOD_MS 3000

// The stall line's width, so the banner over it reads as a divider.
#define BANNER_WIDTH 79

enum { CACHE_L1I, CACHE_L1D, CACHE_L2, NCACHES };

static const char *const cache_node[NCACHES] = {
    "machdep.cache.l1i.all", "machdep.cache.l1d.all", "machdep.cache.l2.all"
};

struct counters
{
    uint64_t cpu[CPU_NPERFCTR];
    uint64_t cache[NCACHES][CACHE_NPERFCTR];
};

static boolean checked;
static boolean enabled;
static struct counters last;
static int last_ms;
static int frames;

// A full read of one bulk node, n counters long.
static boolean
read_node(const char *name, uint64_t *v, size_t n)
{
    size_t len = n * sizeof(*v);

    return sysctlbyname(name, v, &len, NULL, 0) == 0
        && len == n * sizeof(*v);
}

static boolean
read_counters(struct counters *c)
{
    int i;

    if (!read_node("machdep.cpu.all", c->cpu, CPU_NPERFCTR))
    {
        return false;
    }
    for (i = 0; i < NCACHES; i++)
    {
        if (!read_node(cache_node[i], c->cache[i], CACHE_NPERFCTR))
        {
            return false;
        }
    }

    return true;
}

// The kernel zero-extends the 32-bit counters; the difference modulo 2^32
// is right across a wrap.
static uint64_t
delta(uint64_t before, uint64_t after)
{
    return (after - before) & 0xffffffffu;
}

static double
pct(uint64_t part, uint64_t whole)
{
    return whole != 0 ? 100.0 * (double) part / (double) whole : 0.0;
}

static double
stall_pct(const struct counters *now, int idx, uint64_t cycles)
{
    return pct(delta(last.cpu[idx], now->cpu[idx]), cycles);
}

// Reads and writes together, as the cache counts them.
static double
hit_pct(const struct counters *now, int cache)
{
    const uint64_t *b = last.cache[cache], *a = now->cache[cache];
    uint64_t hits, misses;

    hits = delta(b[CACHE_PERF_READ_HITS], a[CACHE_PERF_READ_HITS])
         + delta(b[CACHE_PERF_WRITE_HITS], a[CACHE_PERF_WRITE_HITS]);
    misses = delta(b[CACHE_PERF_READ_MISSES], a[CACHE_PERF_READ_MISSES])
           + delta(b[CACHE_PERF_WRITE_MISSES], a[CACHE_PERF_WRITE_MISSES]);

    return pct(hits, hits + misses);
}

static void
perfctr_init(void)
{
    //!
    // @category obscure
    //
    // Every three seconds, print the frame rate, CPI, stall breakdown and
    // cache hit rates the hardware counters give for the frames since the
    // previous print.  The text goes to stdout, which a console in
    // graphics mode discards: run from a serial or network login, or
    // redirect it.
    //
    if (!M_ParmExists("-perfctr"))
    {
        return;
    }

    if (!read_counters(&last))
    {
        printf("-perfctr: the kernel offers no machdep.cpu/cache counters\n");
        return;
    }

    last_ms = I_GetTimeMS();
    enabled = true;
}

// Print what accrued over the last ms milliseconds and n frames, and make
// this reading the baseline for the next.
static void
perfctr_dump(int ms, int n)
{
    struct counters now;
    char banner[BANNER_WIDTH + 1];
    uint64_t cycles, insns;
    int len;

    if (!read_counters(&now))
    {
        printf("-perfctr: reading the counters failed; no more summaries\n");
        enabled = false;
        return;
    }

    cycles = delta(last.cpu[CPU_PERF_CYCLES], now.cpu[CPU_PERF_CYCLES]);
    insns = delta(last.cpu[CPU_PERF_INSNS], now.cpu[CPU_PERF_INSNS]);

    // Built whole because stdout is unbuffered: padding it a character at
    // a time would be a write for each.
    len = snprintf(banner, sizeof(banner), "== %.2f fps, CPI %.2f ",
                   1000.0 * n / ms,
                   insns != 0 ? (double) cycles / (double) insns : 0.0);
    if (len < BANNER_WIDTH)
    {
        memset(banner + len, '=', BANNER_WIDTH - len);
    }
    banner[BANNER_WIDTH] = '\0';
    puts(banner);

    printf("stalls funit %4.1f%% ifetch %4.1f%% load %4.1f%% store %4.1f%%"
           " hazard %4.1f%% flush %4.1f%%\n",
           stall_pct(&now, CPU_PERF_STALL_FUNIT, cycles),
           stall_pct(&now, CPU_PERF_STALL_IFETCH, cycles),
           stall_pct(&now, CPU_PERF_STALL_LOAD, cycles),
           stall_pct(&now, CPU_PERF_STALL_STORE, cycles),
           stall_pct(&now, CPU_PERF_STALL_HAZARD, cycles),
           stall_pct(&now, CPU_PERF_STALL_FLUSH, cycles));
    printf("hits   L1I %4.1f%%  L1D %4.1f%%  L2 %4.1f%%\n",
           hit_pct(&now, CACHE_L1I),
           hit_pct(&now, CACHE_L1D),
           hit_pct(&now, CACHE_L2));

    last = now;
}

// The engine calls this once a frame, before running tics and drawing.
// Nothing else on this port needs the hook.
//
// The first window opens at the first frame, so it carries the first
// level's load.
void
I_StartFrame(void)
{
    int now;

    if (!checked)
    {
        checked = true;
        perfctr_init();
    }
    if (!enabled)
    {
        return;
    }

    now = I_GetTimeMS();
    if (now - last_ms >= PERFCTR_PERIOD_MS)
    {
        perfctr_dump(now - last_ms, frames);
        last_ms = now;
        frames = 0;
    }
    frames++;
}

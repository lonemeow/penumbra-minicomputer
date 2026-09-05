#include <sys/sysctl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <machine/sysreg.h>

#include "config.h"
#include "doomtype.h"
#include "r_phase.h"

static const char *phase_name[R_PHASE_COUNT] = {
    "rest", "setup", "bsp", "planes", "masked", "wall"
};

static uint64_t acc_cyc[R_PHASE_COUNT];
static uint64_t acc_insn[R_PHASE_COUNT];
static uint64_t frames;

unsigned long r_counts[R_CNT_COUNT];
unsigned long r_wall_seen, r_wall_sampled;

static const char *count_name[R_CNT_COUNT] = {
    "column calls", "column px", "span calls", "span px",
    "FixedDiv", "FixedMul",
    "BSP nodes", "subsectors", "AddLine", "StoreWallRange",
    "FindPlane", "MapPlane", "MakeSpans", "visplanes",
    "ProjectSprite",
    "PointToAngle", "CheckBBox",
    "ClipSolid", "ClipPass",
    "solidseg scan", "solidseg move", "SegLoop iter"
};

static int current = R_PHASE_REST;
static uint32_t last_cyc, last_insn;
static int started;

// sysctlbyname walks the sysctl tree by name on every call, which costs
// tens of thousands of instructions -- enough to swamp what is being
// measured.  Resolve the name to a numeric MIB once and read by MIB after.
static int cpu_mib[CTL_MAXNAME];
static u_int cpu_miblen;

static int
read_cpu(uint32_t *cyc, uint32_t *insn)
{
    uint64_t v[CPU_NPERFCTR];
    size_t len = sizeof(v);

    if (cpu_miblen == 0)
    {
        size_t n = CTL_MAXNAME;

        if (sysctlnametomib("machdep.cpu.all", cpu_mib, &n) != 0)
        {
            return 0;
        }
        cpu_miblen = (u_int) n;
    }

    if (sysctl(cpu_mib, cpu_miblen, v, &len, NULL, 0) != 0)
    {
        return 0;
    }

    *cyc = (uint32_t) v[CPU_PERF_CYCLES];
    *insn = (uint32_t) v[CPU_PERF_INSNS];

    return 1;
}

static void
report(void)
{
    uint64_t tc = 0, ti = 0;
    int p;

    if (frames == 0)
    {
        return;
    }

    for (p = 0; p < R_PHASE_COUNT; p++)
    {
        tc += acc_cyc[p];
        ti += acc_insn[p];
    }
    if (tc == 0)
    {
        return;
    }

    printf("\nrenderer phases over %llu frames\n", (unsigned long long) frames);
    printf("  %-8s %12s %12s %7s\n", "phase", "kcyc/frame", "kinsn/frame", "% cyc");
    for (p = 0; p < R_PHASE_COUNT; p++)
    {
        printf("  %-8s %12llu %12llu %6.1f%%\n", phase_name[p],
               (unsigned long long) (acc_cyc[p] / frames / 1000),
               (unsigned long long) (acc_insn[p] / frames / 1000),
               100.0 * (double) acc_cyc[p] / (double) tc);
    }
    printf("  %-8s %12llu %12llu\n", "TOTAL",
           (unsigned long long) (tc / frames / 1000),
           (unsigned long long) (ti / frames / 1000));

    if (r_wall_sampled)
    {
        printf("\n  wall phase sampled %lu of %lu calls; scaled estimate"
               " %llu kinsn/frame\n",
               r_wall_sampled, r_wall_seen,
               (unsigned long long) (acc_insn[R_PHASE_WALL] * r_wall_seen
                                     / r_wall_sampled / frames / 1000));
    }

    printf("\nper frame\n");
    for (p = 0; p < R_CNT_COUNT; p++)
    {
        printf("  %-16s %8lu\n", count_name[p],
               r_counts[p] / (unsigned long) frames);
    }
}

void
R_PhaseMark(int phase)
{
    uint32_t cyc, insn;

    if (!read_cpu(&cyc, &insn))
    {
        return;
    }

    if (started)
    {
        // Unsigned wrap gives the right delta across a counter rollover.
        acc_cyc[current] += (uint32_t) (cyc - last_cyc);
        acc_insn[current] += (uint32_t) (insn - last_insn);
    }
    else
    {
        atexit(report);
        started = 1;
    }

    if (phase == R_PHASE_SETUP)
    {
        frames++;
    }

    current = phase;
    last_cyc = cyc;
    last_insn = insn;
}

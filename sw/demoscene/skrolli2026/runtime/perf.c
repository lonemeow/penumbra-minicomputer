/* perf.c — CPU performance counters. */

#include "rt.h"
#include "penumbra.h"

void rt_perf_sample(rt_perf_t *out)
{
    out->cycles       = penumbra_read_sysreg(SYSDEV_CPU, CPU_CYCLES);
    out->retired      = penumbra_read_sysreg(SYSDEV_CPU, CPU_INSNS_RETIRED);
    out->stall_funit  = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_FUNIT);
    out->stall_ifetch = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_IFETCH);
    out->stall_load   = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_LOAD);
    out->stall_store  = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_STORE);
    out->stall_hazard = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_HAZARD);
    out->stall_flush  = penumbra_read_sysreg(SYSDEV_CPU, CPU_STALL_FLUSH);
}

uint32_t rt_cycles(void)
{
    return penumbra_read_sysreg(SYSDEV_CPU, CPU_CYCLES);
}

uint32_t rt_perf_delta(const rt_perf_t *a, const rt_perf_t *b,
                       uint32_t out[RT_PERF_BUCKETS])
{
    out[0] = b->retired      - a->retired;
    out[1] = b->stall_load   - a->stall_load;
    out[2] = b->stall_store  - a->stall_store;
    out[3] = b->stall_ifetch - a->stall_ifetch;
    out[4] = b->stall_funit  - a->stall_funit;
    out[5] = b->stall_hazard - a->stall_hazard;
    out[6] = b->stall_flush  - a->stall_flush;

    return b->cycles - a->cycles;
}

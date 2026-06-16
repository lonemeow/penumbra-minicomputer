/*	$NetBSD$	*/

/*
 * CPU performance counter sysctl interface.
 *
 * Exposes the free-running CPU counters (SYSDEV_CPU regs 5-12) and the
 * static CPU clock frequency under machdep.cpu:
 *
 *	machdep.cpu.model		CPU name string (e.g. "Penumbra/2")
 *	machdep.cpu.cycles		free-running cycle counter (RDSYS)
 *	machdep.cpu.insns_retired	free-running retired-instruction counter
 *	machdep.cpu.stall_funit		cycles stalled on a multi-cycle exec unit
 *	machdep.cpu.stall_ifetch	cycles stalled on instruction fetch
 *	machdep.cpu.stall_load		cycles stalled on a data read miss-fill
 *	machdep.cpu.stall_store		cycles stalled on a data write
 *	machdep.cpu.stall_hazard	cycles stalled by a pipeline interlock
 *	machdep.cpu.stall_flush		cycles lost to a front-end redirect/fill
 *	machdep.cpu.freq		CPU clock in Hz (static, latched at boot)
 *	machdep.cpu.all			all eight counters in one read (bulk, struct)
 *
 * cycles/insns_retired each have a custom read handler that issues a
 * single RDSYS at read time — real-time accurate, no kernel-side
 * caching or sampling.  Together they give CPI = d(cycles)/d(insns)
 * and, against freq, instructions/sec (MIPS); see machdep.cache.* for
 * the matching cache hit/miss counters.
 *
 * Wrap: cycles and insns_retired are free-running uint32_t that wrap
 * every ~2.8 min at 25 MHz.  The leaves zero-extend into uint64_t
 * (CTLTYPE_QUAD) so display stays unsigned, but the underlying wrap is
 * unchanged — callers measuring intervals longer than the wrap period
 * must sample often enough to detect it (the penmon monitor samples at
 * ~1 Hz and corrects per-interval deltas).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <machine/cpu.h>
#include <machine/sysreg.h>

/*
 * RDSYS encodes (dev, reg) as immediate operands — runtime dispatch on
 * those fields isn't possible, so one tiny read function per counter
 * lets the compiler emit the right RDSYS for each.  Mirrors the macro
 * in cache_perfctrs.c.
 */
#define DEFINE_PERFCTR_READ(name, dev, reg)				\
static int								\
sysctl_cpu_##name(SYSCTLFN_ARGS)					\
{									\
	struct sysctlnode node;						\
	uint64_t val;							\
	uint32_t raw;							\
									\
	__asm__ volatile("rdsys %0, %1, %2"				\
	    : "=r"(raw) : "i"(dev), "i"(reg));				\
	val = (uint64_t)raw;						\
									\
	node = *rnode;							\
	node.sysctl_data = &val;					\
	return sysctl_lookup(SYSCTLFN_CALL(&node));			\
}

DEFINE_PERFCTR_READ(cycles,        SYSDEV_CPU, CPU_CYCLES)
DEFINE_PERFCTR_READ(insns_retired, SYSDEV_CPU, CPU_INSNS_RETIRED)
DEFINE_PERFCTR_READ(stall_funit,   SYSDEV_CPU, CPU_STALL_FUNIT)
DEFINE_PERFCTR_READ(stall_ifetch,  SYSDEV_CPU, CPU_STALL_IFETCH)
DEFINE_PERFCTR_READ(stall_load,    SYSDEV_CPU, CPU_STALL_LOAD)
DEFINE_PERFCTR_READ(stall_store,   SYSDEV_CPU, CPU_STALL_STORE)
DEFINE_PERFCTR_READ(stall_hazard,  SYSDEV_CPU, CPU_STALL_HAZARD)
DEFINE_PERFCTR_READ(stall_flush,   SYSDEV_CPU, CPU_STALL_FLUSH)

#undef DEFINE_PERFCTR_READ

/*
 * Clock frequency is static — latched into cpu_clock_freq_hz at boot
 * (cpu.c) from SYSDEV_MACH / MACH_CPU_FREQ.  Zero-extend the uint32_t
 * global into a uint64_t QUAD leaf for a uniform display type.
 */
static int
sysctl_cpu_freq(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	uint64_t val = (uint64_t)cpu_clock_freq_hz;

	node = *rnode;
	node.sysctl_data = &val;
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

/*
 * Bulk read: every CPU perfctr in one call.  A poller (e.g. penmon)
 * reads machdep.cpu.all once per refresh instead of one sysctl per
 * counter, cutting syscall overhead.  The reads execute back-to-back;
 * like reading the leaves individually this is not an atomic snapshot,
 * but it is tighter (no syscall boundaries between counters).
 */
static int
sysctl_cpu_all(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	uint64_t v[CPU_NPERFCTR];

#define READ_CTR(reg) ({						\
	uint32_t __raw;							\
	__asm__ volatile("rdsys %0, %1, %2"				\
	    : "=r"(__raw) : "i"(SYSDEV_CPU), "i"(reg));			\
	(uint64_t)__raw; })
	v[CPU_PERF_CYCLES]       = READ_CTR(CPU_CYCLES);
	v[CPU_PERF_INSNS]        = READ_CTR(CPU_INSNS_RETIRED);
	v[CPU_PERF_STALL_FUNIT]  = READ_CTR(CPU_STALL_FUNIT);
	v[CPU_PERF_STALL_IFETCH] = READ_CTR(CPU_STALL_IFETCH);
	v[CPU_PERF_STALL_LOAD]   = READ_CTR(CPU_STALL_LOAD);
	v[CPU_PERF_STALL_STORE]  = READ_CTR(CPU_STALL_STORE);
	v[CPU_PERF_STALL_HAZARD] = READ_CTR(CPU_STALL_HAZARD);
	v[CPU_PERF_STALL_FLUSH]  = READ_CTR(CPU_STALL_FLUSH);
#undef READ_CTR

	node = *rnode;
	node.sysctl_data = v;
	node.sysctl_size = sizeof(v);
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

SYSCTL_SETUP(sysctl_cpu_perfctrs_setup,
    "machdep.cpu.* — CPU performance counters")
{
	const struct sysctlnode *cpu_node = NULL;

	/*
	 * machdep itself is created by init_sysctl_base.c — don't
	 * recreate it.  Add machdep.cpu as a child of CTL_MACHDEP.
	 */
	sysctl_createv(clog, 0, NULL, &cpu_node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "cpu",
	    SYSCTL_DESCR("CPU performance counters"),
	    NULL, 0, NULL, 0,
	    CTL_MACHDEP, CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_STRING, "model",
	    SYSCTL_DESCR("CPU model name (SYSDEV_CPU identity, e.g. Penumbra/2)"),
	    NULL, 0, cpu_model_name, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "cycles",
	    SYSCTL_DESCR("Free-running CPU cycle counter"),
	    sysctl_cpu_cycles, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "insns_retired",
	    SYSCTL_DESCR("Free-running retired-instruction counter"),
	    sysctl_cpu_insns_retired, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_funit",
	    SYSCTL_DESCR("Cycles stalled on a multi-cycle execution unit"),
	    sysctl_cpu_stall_funit, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_ifetch",
	    SYSCTL_DESCR("Cycles stalled on instruction fetch"),
	    sysctl_cpu_stall_ifetch, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_load",
	    SYSCTL_DESCR("Cycles stalled on a data read miss-fill"),
	    sysctl_cpu_stall_load, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_store",
	    SYSCTL_DESCR("Cycles stalled on a data write"),
	    sysctl_cpu_stall_store, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_hazard",
	    SYSCTL_DESCR("Cycles stalled by a pipeline interlock (hazard)"),
	    sysctl_cpu_stall_hazard, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "stall_flush",
	    SYSCTL_DESCR("Cycles lost to a front-end redirect or fill bubble"),
	    sysctl_cpu_stall_flush, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_QUAD, "freq",
	    SYSCTL_DESCR("CPU clock frequency in Hz"),
	    sysctl_cpu_freq, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cpu_node, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_STRUCT, "all",
	    SYSCTL_DESCR("All CPU perfctrs in one read: uint64_t[CPU_NPERFCTR]"),
	    sysctl_cpu_all, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
}

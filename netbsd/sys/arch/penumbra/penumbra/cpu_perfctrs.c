/*	$NetBSD$	*/

/*
 * CPU performance counter sysctl interface.
 *
 * Exposes the free-running CPU counters (SYSDEV_CPU regs 5-6) and the
 * static CPU clock frequency under machdep.cpu:
 *
 *	machdep.cpu.cycles		free-running cycle counter (RDSYS)
 *	machdep.cpu.insns_retired	free-running retired-instruction counter
 *	machdep.cpu.freq		CPU clock in Hz (static, latched at boot)
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
	    CTLTYPE_QUAD, "freq",
	    SYSCTL_DESCR("CPU clock frequency in Hz"),
	    sysctl_cpu_freq, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
}

/*	$NetBSD$	*/

/*
 * Cache performance counter sysctl interface.
 *
 * Exposes the unified-layout cache perfctrs (regs 10-13 on every
 * SYSDEV_*CACHE) under machdep.cache.{l1d,l1i,l2}.{read,write}_{hits,misses}.
 * Each leaf has a custom read handler that issues a single RDSYS at
 * read time — real-time accurate, no kernel-side caching or sampling.
 * Each cache also exposes machdep.cache.<dev>.all, a bulk read of all
 * four counters in one syscall (uint64_t[CACHE_NPERFCTR]) for pollers.
 *
 * Read via standard tools:
 *
 *	$ sysctl machdep.cache
 *	machdep.cache.l1d.read_hits = 1892991
 *	machdep.cache.l1d.read_misses = 100411
 *	...
 *
 * Hit/miss classification is by the tag check at the moment of access
 * — definition is policy-invariant across every write-policy /
 * allocation-policy combination.  See doc/system/sysregs.md for the
 * full per-configuration interpretation table.
 *
 * Wrap: each underlying counter is a free-running uint32_t that wraps
 * every ~170 s at 25 MHz.  The sysctl leaves zero-extend into uint64_t
 * (CTLTYPE_QUAD) so display stays unsigned, but the underlying wrap
 * is unchanged — callers measuring intervals longer than the wrap
 * period must sample often enough to detect it.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <machine/sysreg.h>

/*
 * RDSYS encodes (dev, reg) as immediate operands in the instruction —
 * a runtime dispatch on those fields isn't possible.  A macro
 * generates one tiny read function per counter; the compiler emits
 * the right RDSYS for each.
 */
#define DEFINE_PERFCTR_READ(name, dev, reg)				\
static int								\
sysctl_cache_##name(SYSCTLFN_ARGS)					\
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

DEFINE_PERFCTR_READ(l1d_read_hits,    SYSDEV_L1_DCACHE, CACHE_READ_HITS)
DEFINE_PERFCTR_READ(l1d_read_misses,  SYSDEV_L1_DCACHE, CACHE_READ_MISSES)
DEFINE_PERFCTR_READ(l1d_write_hits,   SYSDEV_L1_DCACHE, CACHE_WRITE_HITS)
DEFINE_PERFCTR_READ(l1d_write_misses, SYSDEV_L1_DCACHE, CACHE_WRITE_MISSES)
DEFINE_PERFCTR_READ(l1i_read_hits,    SYSDEV_L1_ICACHE, CACHE_READ_HITS)
DEFINE_PERFCTR_READ(l1i_read_misses,  SYSDEV_L1_ICACHE, CACHE_READ_MISSES)
DEFINE_PERFCTR_READ(l1i_write_hits,   SYSDEV_L1_ICACHE, CACHE_WRITE_HITS)
DEFINE_PERFCTR_READ(l1i_write_misses, SYSDEV_L1_ICACHE, CACHE_WRITE_MISSES)
DEFINE_PERFCTR_READ(l2_read_hits,     SYSDEV_L2_CACHE,  CACHE_READ_HITS)
DEFINE_PERFCTR_READ(l2_read_misses,   SYSDEV_L2_CACHE,  CACHE_READ_MISSES)
DEFINE_PERFCTR_READ(l2_write_hits,    SYSDEV_L2_CACHE,  CACHE_WRITE_HITS)
DEFINE_PERFCTR_READ(l2_write_misses,  SYSDEV_L2_CACHE,  CACHE_WRITE_MISSES)

#undef DEFINE_PERFCTR_READ

/*
 * Bulk read: a cache's four counters in one call.  A poller (penmon)
 * reads machdep.cache.<dev>.all once per cache per refresh instead of
 * four sysctls.  The reads execute back-to-back — not an atomic
 * snapshot, same caveat as the individual leaves, but tighter.
 */
#define READ_CACHE_CTR(dev, reg) ({					\
	uint32_t __raw;							\
	__asm__ volatile("rdsys %0, %1, %2"				\
	    : "=r"(__raw) : "i"(dev), "i"(reg));			\
	(uint64_t)__raw; })

#define DEFINE_CACHE_ALL(name, dev)					\
static int								\
sysctl_cache_##name##_all(SYSCTLFN_ARGS)				\
{									\
	struct sysctlnode node;						\
	uint64_t v[CACHE_NPERFCTR];					\
									\
	v[CACHE_PERF_READ_HITS]    = READ_CACHE_CTR(dev, CACHE_READ_HITS); \
	v[CACHE_PERF_READ_MISSES]  = READ_CACHE_CTR(dev, CACHE_READ_MISSES); \
	v[CACHE_PERF_WRITE_HITS]   = READ_CACHE_CTR(dev, CACHE_WRITE_HITS); \
	v[CACHE_PERF_WRITE_MISSES] = READ_CACHE_CTR(dev, CACHE_WRITE_MISSES); \
									\
	node = *rnode;							\
	node.sysctl_data = v;						\
	node.sysctl_size = sizeof(v);					\
	return sysctl_lookup(SYSCTLFN_CALL(&node));			\
}

DEFINE_CACHE_ALL(l1d, SYSDEV_L1_DCACHE)
DEFINE_CACHE_ALL(l1i, SYSDEV_L1_ICACHE)
DEFINE_CACHE_ALL(l2,  SYSDEV_L2_CACHE)

#undef DEFINE_CACHE_ALL
#undef READ_CACHE_CTR

/*
 * Helper to keep the four leaf creates per cache from being twelve
 * copies of the same boilerplate.  Each leaf is read-only CTLTYPE_QUAD
 * with its own custom read handler.
 */
#define CREATE_LEAF(parent, name, fn, descr)				\
	sysctl_createv(clog, 0, (parent), NULL,				\
	    CTLFLAG_PERMANENT,						\
	    CTLTYPE_QUAD, (name),					\
	    SYSCTL_DESCR(descr),					\
	    (fn), 0, NULL, 0,						\
	    CTL_CREATE, CTL_EOL)

/* The bulk per-cache counterpart to CREATE_LEAF: one CTLTYPE_STRUCT
 * "all" leaf returning uint64_t[CACHE_NPERFCTR]. */
#define CREATE_ALL(parent, fn)						\
	sysctl_createv(clog, 0, (parent), NULL,				\
	    CTLFLAG_PERMANENT,						\
	    CTLTYPE_STRUCT, "all",					\
	    SYSCTL_DESCR("All four counters in one read: "		\
		"uint64_t[CACHE_NPERFCTR]"),				\
	    (fn), 0, NULL, 0,						\
	    CTL_CREATE, CTL_EOL)

SYSCTL_SETUP(sysctl_cache_perfctrs_setup,
    "machdep.cache.* — cache performance counters")
{
	const struct sysctlnode *cache_node = NULL;
	const struct sysctlnode *l1d_node = NULL;
	const struct sysctlnode *l1i_node = NULL;
	const struct sysctlnode *l2_node = NULL;

	/*
	 * machdep itself is created by init_sysctl_base.c — don't
	 * recreate it.  Add machdep.cache as a child of CTL_MACHDEP,
	 * then the per-cache subtrees as children of cache.
	 */
	sysctl_createv(clog, 0, NULL, &cache_node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "cache",
	    SYSCTL_DESCR("Cache performance counters"),
	    NULL, 0, NULL, 0,
	    CTL_MACHDEP, CTL_CREATE, CTL_EOL);

	sysctl_createv(clog, 0, &cache_node, &l1d_node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "l1d",
	    SYSCTL_DESCR("L1 D-cache"),
	    NULL, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	CREATE_LEAF(&l1d_node, "read_hits",
	    sysctl_cache_l1d_read_hits,
	    "Reads that hit a valid line");
	CREATE_LEAF(&l1d_node, "read_misses",
	    sysctl_cache_l1d_read_misses,
	    "Reads that missed");
	CREATE_LEAF(&l1d_node, "write_hits",
	    sysctl_cache_l1d_write_hits,
	    "Writes that hit a valid line");
	CREATE_LEAF(&l1d_node, "write_misses",
	    sysctl_cache_l1d_write_misses,
	    "Writes that missed");
	CREATE_ALL(&l1d_node, sysctl_cache_l1d_all);

	sysctl_createv(clog, 0, &cache_node, &l1i_node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "l1i",
	    SYSCTL_DESCR("L1 I-cache (write_* always 0 — never sees stores)"),
	    NULL, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	CREATE_LEAF(&l1i_node, "read_hits",
	    sysctl_cache_l1i_read_hits,
	    "Instruction fetches that hit a valid line");
	CREATE_LEAF(&l1i_node, "read_misses",
	    sysctl_cache_l1i_read_misses,
	    "Instruction fetches that missed");
	CREATE_LEAF(&l1i_node, "write_hits",
	    sysctl_cache_l1i_write_hits,
	    "Writes that hit (always 0 for I-cache)");
	CREATE_LEAF(&l1i_node, "write_misses",
	    sysctl_cache_l1i_write_misses,
	    "Writes that missed (always 0 for I-cache)");
	CREATE_ALL(&l1i_node, sysctl_cache_l1i_all);

	sysctl_createv(clog, 0, &cache_node, &l2_node,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "l2",
	    SYSCTL_DESCR("L2 unified cache"),
	    NULL, 0, NULL, 0,
	    CTL_CREATE, CTL_EOL);
	CREATE_LEAF(&l2_node, "read_hits",
	    sysctl_cache_l2_read_hits,
	    "Reads that hit a valid line");
	CREATE_LEAF(&l2_node, "read_misses",
	    sysctl_cache_l2_read_misses,
	    "Reads that missed");
	CREATE_LEAF(&l2_node, "write_hits",
	    sysctl_cache_l2_write_hits,
	    "Writes that hit a valid line");
	CREATE_LEAF(&l2_node, "write_misses",
	    sysctl_cache_l2_write_misses,
	    "Writes that missed");
	CREATE_ALL(&l2_node, sysctl_cache_l2_all);
}

#undef CREATE_LEAF
#undef CREATE_ALL

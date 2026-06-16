/*	$NetBSD$	*/
/*
 * penmon sampling layer: read every counter via sysctl, then turn a
 * pair of snapshots into per-interval rates.
 */
#include "penmon.h"

#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#include <machine/sysreg.h>	/* CPU_PERF_* / CPU_NPERFCTR bulk-read layout */
#include <stdio.h>
#include <string.h>

/* A machdep.* leaf is easiest to read by name; helper returns 0 on
 * failure so an un-patched kernel just shows zeros instead of aborting. */
static uint64_t
read_quad(const char *name)
{
	uint64_t v = 0;
	size_t len = sizeof(v);

	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return 0;
	return v;
}

static void
read_cache(const char *prefix, struct cache_ctr *c)
{
	uint64_t v[CACHE_NPERFCTR];
	size_t len = sizeof(v);
	char nm[64];

	/* One syscall for all four counters via machdep.cache.<dev>.all;
	 * fall back to the individual leaves on an older kernel. */
	snprintf(nm, sizeof(nm), "%s.all", prefix);
	if (sysctlbyname(nm, v, &len, NULL, 0) == 0 && len == sizeof(v)) {
		c->read_hits    = v[CACHE_PERF_READ_HITS];
		c->read_misses  = v[CACHE_PERF_READ_MISSES];
		c->write_hits   = v[CACHE_PERF_WRITE_HITS];
		c->write_misses = v[CACHE_PERF_WRITE_MISSES];
		return;
	}

	snprintf(nm, sizeof(nm), "%s.read_hits", prefix);
	c->read_hits = read_quad(nm);
	snprintf(nm, sizeof(nm), "%s.read_misses", prefix);
	c->read_misses = read_quad(nm);
	snprintf(nm, sizeof(nm), "%s.write_hits", prefix);
	c->write_hits = read_quad(nm);
	snprintf(nm, sizeof(nm), "%s.write_misses", prefix);
	c->write_misses = read_quad(nm);
}

int
read_cpu_freq(uint64_t *hz)
{
	uint64_t v = read_quad("machdep.cpu.freq");

	if (v == 0)
		return 0;
	*hz = v;
	return 1;
}

void
read_snapshot(struct snapshot *s)
{
	int mib[2];
	size_t len;

	memset(s, 0, sizeof(*s));
	clock_gettime(CLOCK_MONOTONIC, &s->t);

	/* All CPU perfctrs in one syscall (machdep.cpu.all) — one read per
	 * refresh instead of six.  Fall back to the individual leaves if the
	 * bulk node is missing (older kernel). */
	{
		uint64_t v[CPU_NPERFCTR];
		size_t len = sizeof(v);

		if (sysctlbyname("machdep.cpu.all", v, &len, NULL, 0) == 0 &&
		    len == sizeof(v)) {
			s->cycles       = v[CPU_PERF_CYCLES];
			s->insns        = v[CPU_PERF_INSNS];
			s->stall_funit  = v[CPU_PERF_STALL_FUNIT];
			s->stall_ifetch = v[CPU_PERF_STALL_IFETCH];
			s->stall_load   = v[CPU_PERF_STALL_LOAD];
			s->stall_store  = v[CPU_PERF_STALL_STORE];
			s->stall_hazard = v[CPU_PERF_STALL_HAZARD];
			s->stall_flush  = v[CPU_PERF_STALL_FLUSH];
		} else {
			s->cycles       = read_quad("machdep.cpu.cycles");
			s->insns        = read_quad("machdep.cpu.insns_retired");
			s->stall_funit  = read_quad("machdep.cpu.stall_funit");
			s->stall_ifetch = read_quad("machdep.cpu.stall_ifetch");
			s->stall_load   = read_quad("machdep.cpu.stall_load");
			s->stall_store  = read_quad("machdep.cpu.stall_store");
			s->stall_hazard = read_quad("machdep.cpu.stall_hazard");
			s->stall_flush  = read_quad("machdep.cpu.stall_flush");
		}
	}

	read_cache("machdep.cache.l1i", &s->l1i);
	read_cache("machdep.cache.l1d", &s->l1d);
	read_cache("machdep.cache.l2",  &s->l2);

	/* vm.uvmexp2 — cumulative system-activity counters (page faults,
	 * interrupts, syscalls, context switches, forks).  One struct read
	 * feeds several per-interval rates. */
	{
		struct uvmexp_sysctl u;
		size_t ulen = sizeof(u);
		int vmib[2];

		vmib[0] = CTL_VM;
		vmib[1] = VM_UVMEXP2;
		if (sysctl(vmib, 2, &u, &ulen, NULL, 0) == 0) {
			s->faults   = (uint64_t)u.faults;
			s->intrs    = (uint64_t)u.intrs;
			s->syscalls = (uint64_t)u.syscalls;
			s->swtch    = (uint64_t)u.swtch;
			s->forks    = (uint64_t)u.forks;
		}
	}

	/* kern.cp_time is a fixed 5-entry uint64 array of clock ticks. */
	mib[0] = CTL_KERN;
	mib[1] = KERN_CP_TIME;
	len = sizeof(s->cp_time);
	if (sysctl(mib, 2, s->cp_time, &len, NULL, 0) != 0)
		memset(s->cp_time, 0, sizeof(s->cp_time));
}

/*
 * counter_delta — events between two reads of a free-running 32-bit
 * hardware counter.
 *
 * The Penumbra cycle/instruction/cache counters are 32 bits wide and
 * free-running: they wrap back to 0 at 2^32.  The kernel zero-extends
 * each read into the low 32 bits of a uint64_t, so both `prev` and
 * `cur` are always in [0, 2^32), but `cur` may be numerically *smaller*
 * than `prev` when the counter wrapped between the two samples.
 *
 * As long as fewer than 2^32 events occurred during the interval — true
 * at penmon's ~1 Hz sampling, since even the fastest counter (cycles at
 * 25 MHz) wraps in ~170 s — the true event count is the unsigned
 * difference taken modulo 2^32.
 */
uint64_t
counter_delta(uint64_t prev, uint64_t cur)
{
	if (prev > 0xFFFFFFFF || cur > 0xFFFFFFFF) {
		return cur - prev;
	} else {
		return (cur - prev) & 0xFFFFFFFF;
	}
}

/* hits/(hits+misses) as a percentage, 0 when the cache was idle. */
static double
hit_pct(uint64_t hits, uint64_t misses)
{
	uint64_t total = hits + misses;

	return total ? (100.0 * (double)hits) / (double)total : 0.0;
}

static void
cache_rate(const struct cache_ctr *a, const struct cache_ctr *b,
    double dt, struct cache_rate *out)
{
	uint64_t rh = counter_delta(a->read_hits,    b->read_hits);
	uint64_t rm = counter_delta(a->read_misses,  b->read_misses);
	uint64_t wh = counter_delta(a->write_hits,   b->write_hits);
	uint64_t wm = counter_delta(a->write_misses, b->write_misses);

	out->hit_pct = hit_pct(rh + wh, rm + wm);
	out->miss_per_sec = dt > 0 ? (double)(rm + wm) / dt : 0.0;
}

void
compute_rates(const struct snapshot *prev, const struct snapshot *cur,
    uint64_t clk_hz, struct rates *out)
{
	uint64_t dcyc, dins, busy, total, i;
	double dt;

	memset(out, 0, sizeof(*out));

	dt = (double)(cur->t.tv_sec - prev->t.tv_sec) +
	     (double)(cur->t.tv_nsec - prev->t.tv_nsec) / 1e9;
	if (dt <= 0)
		dt = 1e-9;
	out->dt = dt;
	out->clk_mhz = (double)clk_hz / 1e6;

	dcyc = counter_delta(prev->cycles, cur->cycles);
	dins = counter_delta(prev->insns,  cur->insns);
	out->cpi  = dins ? (double)dcyc / (double)dins : 0.0;
	out->mips = (double)dins / 1e6 / dt;

	/* Each stall bucket as a percentage of the interval's cycles.
	 * out is memset to 0 above, so an idle interval (dcyc == 0) leaves
	 * all four at 0. */
	if (dcyc) {
		double dc = (double)dcyc;
		out->stall_funit_pct  =
		    100.0 * counter_delta(prev->stall_funit,  cur->stall_funit)  / dc;
		out->stall_ifetch_pct =
		    100.0 * counter_delta(prev->stall_ifetch, cur->stall_ifetch) / dc;
		out->stall_load_pct   =
		    100.0 * counter_delta(prev->stall_load,   cur->stall_load)   / dc;
		out->stall_store_pct  =
		    100.0 * counter_delta(prev->stall_store,  cur->stall_store)  / dc;
		out->stall_hazard_pct =
		    100.0 * counter_delta(prev->stall_hazard, cur->stall_hazard) / dc;
		out->stall_flush_pct  =
		    100.0 * counter_delta(prev->stall_flush,  cur->stall_flush)  / dc;
	}

	/* CPU time: cp_time is true 64-bit monotonic ticks (no wrap). */
	total = 0;
	for (i = 0; i < 5; i++)
		total += cur->cp_time[i] - prev->cp_time[i];
	busy = total ? total : 1;
	for (i = 0; i < 5; i++)
		out->cpu_pct[i] =
		    100.0 * (double)(cur->cp_time[i] - prev->cp_time[i]) /
		    (double)busy;

	cache_rate(&prev->l1i, &cur->l1i, dt, &out->l1i);
	cache_rate(&prev->l1d, &cur->l1d, dt, &out->l1d);
	cache_rate(&prev->l2,  &cur->l2,  dt, &out->l2);

	/* uvmexp2 activity counters: cumulative 64-bit, no wrap — plain diff. */
	out->faults_per_sec  = (double)(cur->faults   - prev->faults)   / dt;
	out->intr_per_sec    = (double)(cur->intrs    - prev->intrs)    / dt;
	out->syscall_per_sec = (double)(cur->syscalls - prev->syscalls) / dt;
	out->csw_per_sec     = (double)(cur->swtch    - prev->swtch)    / dt;
	out->fork_per_sec    = (double)(cur->forks    - prev->forks)    / dt;
}

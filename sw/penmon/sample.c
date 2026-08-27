/*	$NetBSD$	*/
/*
 * penmon sampling layer: read every counter via sysctl, then turn a
 * pair of snapshots into per-interval rates.
 */
#include "penmon.h"

#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#include <sys/evcnt.h>
#include <stdlib.h>
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

/*
 * Per-source interrupt counters from kern.evcnt — the interrupt-typed
 * event counters, which is what vmstat -i reports.  The reply is a
 * packed sequence of variable-length records: a fixed header followed
 * by the group and name strings, with ev_len giving the record's size
 * in 8-byte units.  Sources are named "group name", matching vmstat's
 * presentation, and only nonzero counters are requested — a device
 * that has never interrupted earns no panel row.
 */
static void
read_intr_counters(struct snapshot *s)
{
	const int mib[4] = { CTL_KERN, KERN_EVCNT, EVCNT_TYPE_INTR,
	    KERN_EVCNT_COUNT_NONZERO };
	const struct evcnt_sysctl *ev;
	const char *end;
	static char *buf;
	static size_t bufcap;
	size_t len = 0;

	s->nirq = 0;

	if (sysctl(mib, __arraycount(mib), NULL, &len, NULL, 0) != 0 ||
	    len == 0)
		return;
	if (len > bufcap) {
		char *nb = realloc(buf, len);

		if (nb == NULL)
			return;
		buf = nb;
		bufcap = len;
	}
	if (sysctl(mib, __arraycount(mib), buf, &len, NULL, 0) != 0)
		return;

	end = buf + len;
	ev = (const struct evcnt_sysctl *)(const void *)buf;
	while ((const char *)ev + sizeof(*ev) <= end &&
	    s->nirq < PENMON_IRQ_MAX) {
		size_t reclen = (size_t)ev->ev_len * 8;
		const char *group = ev->ev_strings;
		const char *name = group + ev->ev_grouplen + 1;

		if (reclen < sizeof(*ev) || (const char *)ev + reclen > end)
			break;
		snprintf(s->irq[s->nirq].name, PENMON_IRQ_NAME, "%s %s",
		    group, name);
		s->irq[s->nirq].count = ev->ev_count;
		s->nirq++;
		ev = (const struct evcnt_sysctl *)(const void *)
		    ((const char *)ev + reclen);
	}
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

int
read_cpu_model(char *buf, size_t bufsz)
{
	size_t len = bufsz;

	if (buf == NULL || bufsz == 0)
		return 0;
	if (sysctlbyname("machdep.cpu.model", buf, &len, NULL, 0) != 0) {
		buf[0] = '\0';		/* un-patched kernel: leave it blank */
		return 0;
	}
	buf[bufsz - 1] = '\0';		/* never trust the source to terminate */
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

	read_intr_counters(s);

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

/*
 * Events observed over the interval expressed as a rate per second.
 *
 * A counter delta is at most 2^33 (two 32-bit counters summed), so scaling
 * by a microsecond in the numerator stays well inside 64 bits and the
 * division still rounds on the full-precision value.
 */
static uint32_t
per_sec(uint64_t events, uint64_t dt_us)
{
	return (uint32_t)divround(events * 1000000, dt_us);
}

static void
cache_rate(const struct cache_ctr *a, const struct cache_ctr *b,
    uint64_t dt_us, struct cache_rate *out)
{
	uint64_t rh = counter_delta(a->read_hits,    b->read_hits);
	uint64_t rm = counter_delta(a->read_misses,  b->read_misses);
	uint64_t wh = counter_delta(a->write_hits,   b->write_hits);
	uint64_t wm = counter_delta(a->write_misses, b->write_misses);
	uint64_t hits = rh + wh, total = rh + wh + rm + wm;

	out->hit_pct = (uint32_t)divround(hits * PCT_FULL, total);
	out->miss_per_sec = per_sec(rm + wm, dt_us);
}

void
compute_rates(const struct snapshot *prev, const struct snapshot *cur,
    uint64_t clk_hz, struct rates *out)
{
	uint64_t dcyc, dins, busy, total, i, dt_us;

	memset(out, 0, sizeof(*out));

	dt_us = (uint64_t)(cur->t.tv_sec - prev->t.tv_sec) * 1000000 +
	    (uint64_t)((cur->t.tv_nsec - prev->t.tv_nsec) / 1000);
	if (dt_us == 0)
		dt_us = 1;		/* the divisor below, never the display */
	out->dt_us = dt_us;
	out->clk_khz = (uint32_t)divround(clk_hz, 1000);

	dcyc = counter_delta(prev->cycles, cur->cycles);
	dins = counter_delta(prev->insns,  cur->insns);
	out->cpi  = (uint32_t)divround(dcyc * CPI_SCALE, dins);
	/* Instructions per microsecond *is* MIPS, so the interval in
	 * microseconds is already the right divisor. */
	out->mips = (uint32_t)divround(dins * MIPS_SCALE, dt_us);

	/* Each stall bucket as a fraction of the interval's cycles.
	 * out is memset to 0 above, so an idle interval (dcyc == 0) leaves
	 * all six at 0. */
	if (dcyc) {
		out->stall_funit_pct  = (uint32_t)divround(
		    counter_delta(prev->stall_funit,  cur->stall_funit)  * PCT_FULL, dcyc);
		out->stall_ifetch_pct = (uint32_t)divround(
		    counter_delta(prev->stall_ifetch, cur->stall_ifetch) * PCT_FULL, dcyc);
		out->stall_load_pct   = (uint32_t)divround(
		    counter_delta(prev->stall_load,   cur->stall_load)   * PCT_FULL, dcyc);
		out->stall_store_pct  = (uint32_t)divround(
		    counter_delta(prev->stall_store,  cur->stall_store)  * PCT_FULL, dcyc);
		out->stall_hazard_pct = (uint32_t)divround(
		    counter_delta(prev->stall_hazard, cur->stall_hazard) * PCT_FULL, dcyc);
		out->stall_flush_pct  = (uint32_t)divround(
		    counter_delta(prev->stall_flush,  cur->stall_flush)  * PCT_FULL, dcyc);
	}

	/* CPU time: cp_time is true 64-bit monotonic ticks (no wrap). */
	total = 0;
	for (i = 0; i < 5; i++)
		total += cur->cp_time[i] - prev->cp_time[i];
	busy = total ? total : 1;
	for (i = 0; i < 5; i++)
		out->cpu_pct[i] = (uint32_t)divround(
		    (cur->cp_time[i] - prev->cp_time[i]) * PCT_FULL, busy);

	cache_rate(&prev->l1i, &cur->l1i, dt_us, &out->l1i);
	cache_rate(&prev->l1d, &cur->l1d, dt_us, &out->l1d);
	cache_rate(&prev->l2,  &cur->l2,  dt_us, &out->l2);

	/* uvmexp2 activity counters: cumulative 64-bit, no wrap — plain diff. */
	out->faults_per_sec  = per_sec(cur->faults   - prev->faults,   dt_us);
	out->intr_per_sec    = per_sec(cur->intrs    - prev->intrs,    dt_us);
	out->syscall_per_sec = per_sec(cur->syscalls - prev->syscalls, dt_us);
	out->csw_per_sec     = per_sec(cur->swtch    - prev->swtch,    dt_us);
	out->fork_per_sec    = per_sec(cur->forks    - prev->forks,    dt_us);

	/* Per-source interrupt rates, matched by name across the two
	 * snapshots: the set changes as devices attach and detach, so
	 * index position carries no meaning between samples. */
	out->nirq = 0;
	for (int k = 0; k < cur->nirq; k++) {
		int j;

		for (j = 0; j < prev->nirq; j++) {
			if (strcmp(cur->irq[k].name, prev->irq[j].name) != 0)
				continue;
			strlcpy(out->irq[out->nirq].name, cur->irq[k].name,
			    PENMON_IRQ_NAME);
			out->irq[out->nirq].per_sec = per_sec(
			    cur->irq[k].count - prev->irq[j].count, dt_us);
			out->nirq++;
			break;
		}
	}

	/* Busiest first — a fixed-height panel shows the top few. */
	for (int k = 1; k < out->nirq; k++) {
		int j = k;

		while (j > 0 && out->irq[j].per_sec > out->irq[j - 1].per_sec) {
			typeof(out->irq[0]) tmp = out->irq[j];

			out->irq[j] = out->irq[j - 1];
			out->irq[j - 1] = tmp;
			j--;
		}
	}
}

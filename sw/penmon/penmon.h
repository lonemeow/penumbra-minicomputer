/*	$NetBSD$	*/
/*
 * penmon — Penumbra hardware-counter system monitor.
 *
 * A top/htop/systat-style live display that pairs the classic OS metrics
 * (CPU busy %, memory, load, per-process
 * CPU) with Penumbra's hardware performance counters (CPI, MIPS, and
 * L1I/L1D/L2 cache hit rates) read straight out of the sysctl tree.
 *
 * Design: every refresh takes one "snapshot" of all counters plus a
 * monotonic timestamp, then derives per-interval *rates* by differencing
 * against the previous snapshot.  Raw counter totals are meaningless on
 * their own — the hardware counters are free-running 32-bit and wrap in
 * ~170 s at 25 MHz — so the tool only ever shows rates over an interval.
 */
#ifndef PENMON_H
#define PENMON_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>

/*
 * Fixed-point scales for every derived value.
 *
 * Penumbra has no FPU, so floating point costs twice over: the soft-float
 * runtime for the arithmetic, and printf's exact decimal converter for the
 * output.  Both dwarf the sampling that produces the numbers, and a
 * dashboard that shows one or two decimal places never needed the range.
 * Rates are therefore carried as scaled integers all the way to snprintf.
 *
 * PCT_FULL is the value of "100%", so a percentage in these units doubles
 * as a bar-fill fraction without a second unit or a conversion.
 */
#define PCT_FULL   1000		/* percentages in tenths of a percent */
#define CPI_SCALE  100		/* cycles per instruction, in hundredths */
#define MIPS_SCALE 100		/* retired MIPS, in hundredths */
#define LOAD_SCALE 100		/* load average, in hundredths */

/* a/b rounded to nearest, 0 when b is 0 — the division every derived rate
 * below is built from. */
static inline uint64_t
divround(uint64_t a, uint64_t b)
{
	return b ? (a + b / 2) / b : 0;
}

/* One cache device's four free-running event counters (zero-extended
 * to 64 bits by the kernel, but the live value still wraps at 2^32). */
struct cache_ctr {
	uint64_t read_hits;
	uint64_t read_misses;
	uint64_t write_hits;
	uint64_t write_misses;
};

/* Per-source interrupt counters (kern.evcnt, EVCNT_TYPE_INTR — the
 * same set vmstat -i reports).  Sources come and go as devices attach,
 * so a snapshot carries its own list and rates are matched by name. */
#define PENMON_IRQ_MAX   16
#define PENMON_IRQ_NAME  28

struct irqsrc {
	char     name[PENMON_IRQ_NAME];	/* "group name", as vmstat -i prints */
	uint64_t count;			/* cumulative */
};

/* A complete reading of every counter at one instant. */
struct snapshot {
	struct timespec t;		/* CLOCK_MONOTONIC at sample time */
	uint64_t cycles;		/* machdep.cpu.cycles */
	uint64_t insns;			/* machdep.cpu.insns_retired */
	uint64_t stall_funit;		/* machdep.cpu.stall_* — per-cause stall */
	uint64_t stall_ifetch;
	uint64_t stall_load;
	uint64_t stall_store;
	uint64_t stall_hazard;		/* pipeline interlock (hazard) */
	uint64_t stall_flush;		/* front-end redirect / fill bubble */
	struct cache_ctr l1i, l1d, l2;	/* machdep.cache.* */
	uint64_t cp_time[5];		/* kern.cp_time: usr,nice,sys,intr,idle */
	uint64_t faults;		/* vm.uvmexp2.faults — cumulative page faults */
	uint64_t intrs;			/* vm.uvmexp2.intrs — hardware interrupts */
	uint64_t syscalls;		/* vm.uvmexp2.syscalls */
	uint64_t swtch;			/* vm.uvmexp2.swtch — context switches */
	uint64_t forks;			/* vm.uvmexp2.forks — process creations */
	struct irqsrc irq[PENMON_IRQ_MAX];
	int      nirq;			/* sources present in this snapshot */
};

/* Per-cache derived rates for display. */
struct cache_rate {
	uint32_t hit_pct;		/* (hits / total accesses), PCT_FULL scale */
	uint32_t miss_per_sec;		/* misses per wall-clock second */
};

/* Everything the renderer needs, derived from two snapshots. */
struct rates {
	uint64_t dt_us;			/* interval length in microseconds */
	uint32_t cpi;			/* cycles / instructions, CPI_SCALE */
	uint32_t mips;			/* instructions retired / s, MIPS_SCALE */
	uint32_t stall_funit_pct;	/* interval cycles stalled by cause,
					 * PCT_FULL scale */
	uint32_t stall_ifetch_pct;
	uint32_t stall_load_pct;
	uint32_t stall_store_pct;
	uint32_t stall_hazard_pct;
	uint32_t stall_flush_pct;
	uint32_t clk_khz;		/* CPU clock (static, machdep.cpu.freq) */
	uint32_t cpu_pct[5];		/* interval in usr,nice,sys,intr,idle,
					 * PCT_FULL scale */
	uint32_t faults_per_sec;	/* page faults per wall-clock second */
	uint32_t intr_per_sec;		/* hardware interrupts / s */
	uint32_t syscall_per_sec;	/* system calls / s */
	uint32_t csw_per_sec;		/* context switches / s */
	uint32_t fork_per_sec;		/* process creations / s */
	struct cache_rate l1i, l1d, l2;
	/* Per-source interrupt rates, highest first.  A source absent
	 * from the previous snapshot (a device that just attached)
	 * contributes no rate until it has been seen twice. */
	struct { char name[PENMON_IRQ_NAME]; uint32_t per_sec; }
		 irq[PENMON_IRQ_MAX];
	int      nirq;
};

/* kern.cp_time index names. */
enum { CP_USER, CP_NICE, CP_SYS, CP_INTR, CP_IDLE };

/* ---- sample.c ---------------------------------------------------------- */

/* Read the static CPU clock (machdep.cpu.freq) once at startup, in Hz.
 * Returns 0 and leaves *hz untouched on failure. */
int  read_cpu_freq(uint64_t *hz);

/* CPU model-name buffer size — matches the kernel's cpu_model_name[17]
 * (16 chars + NUL), the SYSDEV_CPU identity string. */
#define PENMON_MODELLEN 17

/* Read the static CPU model name (machdep.cpu.model) once at startup into
 * buf.  Returns 0 and writes an empty string on failure (un-patched kernel). */
int  read_cpu_model(char *buf, size_t bufsz);

/* Take a full snapshot of all counters + timestamp.  Missing sysctls are
 * left zero so the tool degrades gracefully on an un-patched kernel. */
void read_snapshot(struct snapshot *s);

/* Events between two reads of a free-running 32-bit counter that the
 * kernel zero-extends into 64 bits.  Defined in sample.c. */
uint64_t counter_delta(uint64_t prev, uint64_t cur);

/* Derive display rates from two snapshots and the (static) clock. */
void compute_rates(const struct snapshot *prev, const struct snapshot *cur,
                   uint64_t clk_hz, struct rates *out);

/* Current memory state (a level, not a rate — read fresh each frame). */
struct meminfo {
	uint64_t total_bytes;
	uint64_t active_bytes;
	uint64_t wired_bytes;
	uint64_t free_bytes;
};

/* Read vm.uvmexp2 into *m (zeroed on failure). */
void read_meminfo(struct meminfo *m);

/* Read system uptime in seconds from kern.boottime; 0 on failure. */
long read_uptime(void);

/* Read the 1-minute load average (vm.loadavg) at LOAD_SCALE; 0 on failure.
 * The sysctl carries the kernel's own fixed-point value, so no conversion
 * through a floating-point getloadavg() is needed. */
uint32_t read_loadavg(void);

/* ---- proc.c ---------------------------------------------------------- */

#define PENMON_MAXPROC 128
#define PENMON_COMMLEN 24
#define PENMON_USERLEN 24

struct procinfo {
	int32_t  pid;
	uint32_t pctcpu;		/* decayed kernel average, PCT_FULL scale */
	uint64_t rss_bytes;		/* resident set size */
	char     state;			/* R/S/T/Z/I/... */
	char     comm[PENMON_COMMLEN];
	char     user[PENMON_USERLEN];
};

/* Fill out[] with up to max processes via KERN_PROC2, sorted by %CPU
 * descending.  Returns the number written. */
int read_procs(struct procinfo *out, int max);

/* ---- render.c -------------------------------------------------------- */

/* Rolling history for the sparkline graphs.  A simple ring: head is the
 * index of the most-recent sample, count caps at HIST_LEN. */
#define HIST_LEN 64
struct history {
	uint32_t cpi[HIST_LEN];		/* CPI_SCALE */
	uint32_t l1i[HIST_LEN];		/* hit rates, PCT_FULL scale */
	uint32_t l1d[HIST_LEN];
	uint32_t l2[HIST_LEN];
	int      head;
	int      count;
};

void history_init(struct history *h);
void history_push(struct history *h, const struct rates *r);

/* Draw one full frame into the screen layer (screen.h) and flush it. */
void render_frame(const struct rates *r, const struct history *h,
                  const struct meminfo *mem, uint32_t load1, long uptime_sec,
                  const struct procinfo *procs, int nproc, int interval_ms,
                  const char *cpu_model);

#endif /* PENMON_H */

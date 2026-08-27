/*	$NetBSD$	*/
/*
 * penmon process + memory sampling via the KERN_PROC2 / VM_UVMEXP2
 * sysctl interfaces.
 */
#include "penmon.h"

#include <sys/param.h>		/* FSCALE */
#include <sys/resource.h>	/* struct loadavg */
#include <sys/sysctl.h>
#include <sys/time.h>
#include <uvm/uvm_extern.h>	/* struct uvmexp_sysctl */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* LWP run states (sys/lwp.h values) mapped to the familiar ps letters. */
static char
state_letter(int8_t st)
{
	switch (st) {
	case 1:  return 'I';	/* LSIDL    */
	case 2:  return 'R';	/* LSRUN    */
	case 3:  return 'S';	/* LSSLEEP  */
	case 4:  return 'T';	/* LSSTOP   */
	case 5:  return 'Z';	/* LSZOMB   */
	case 7:  return 'R';	/* LSONPROC */
	case 8:  return 'U';	/* LSSUSPENDED */
	default: return '?';
	}
}

/* qsort: %CPU descending, RSS as tiebreak. */
static int
by_cpu_desc(const void *a, const void *b)
{
	const struct procinfo *pa = a, *pb = b;

	if (pa->pctcpu < pb->pctcpu) return 1;
	if (pa->pctcpu > pb->pctcpu) return -1;
	if (pa->rss_bytes < pb->rss_bytes) return 1;
	if (pa->rss_bytes > pb->rss_bytes) return -1;
	return 0;
}

/*
 * Entries the process-table buffer starts at.  A typical system fits
 * without a retry; beyond that the buffer grows itself and stays grown,
 * so the cost is paid once rather than per refresh.
 */
#define PROC_CAP_INIT 64

/*
 * Read the whole process table into a buffer that outlives the call.
 *
 * KERN_PROC2 reports a buffer it could not fill as ENOMEM, which is
 * enough to size the buffer by retrying: hold it across refreshes and a
 * steady process count costs one syscall.  Asking the kernel for the
 * size first would cost a second every refresh, and that pass is not a
 * cheap count — it walks the whole process list and runs the same
 * per-process permission check as the pass that fills the buffer.
 *
 * Returns the number of entries read and points *entries at them, or -1
 * if the table could not be read.
 */
static int
fetch_kproc2(struct kinfo_proc2 **entries)
{
	static struct kinfo_proc2 *buf;
	static size_t cap;		/* entries buf holds; grows, never shrinks */
	int mib[6];
	size_t want = cap != 0 ? cap : PROC_CAP_INIT;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC2;
	mib[2] = KERN_PROC_ALL;
	mib[3] = 0;
	mib[4] = (int)sizeof(**entries);

	for (;;) {
		size_t len;

		if (want > cap) {
			struct kinfo_proc2 *nb =
			    realloc(buf, want * sizeof(*nb));

			if (nb == NULL)
				return -1;
			buf = nb;
			cap = want;
		}

		len = cap * sizeof(*buf);
		mib[5] = (int)cap;
		if (sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
			*entries = buf;
			return (int)(len / sizeof(*buf));
		}
		if (errno != ENOMEM)
			return -1;

		/* The kernel does not report how much it needed, so close
		 * on it by doubling; the process count bounds the loop. */
		want = cap * 2;
	}
}

int
read_procs(struct procinfo *out, int max)
{
	static size_t pgsz;		/* invariant for the life of the process */
	struct kinfo_proc2 *kp;
	int i, n, count = 0;

	if ((n = fetch_kproc2(&kp)) < 0)
		return 0;
	if (pgsz == 0)
		pgsz = (size_t)sysconf(_SC_PAGESIZE);

	for (i = 0; i < n && count < max; i++) {
		struct procinfo *p = &out[count];

		p->pid = kp[i].p_pid;
		p->pctcpu = (uint32_t)divround(
		    (uint64_t)kp[i].p_pctcpu * PCT_FULL, FSCALE);
		p->rss_bytes = (uint64_t)kp[i].p_vm_rssize * pgsz;
		p->state = state_letter(kp[i].p_stat);

		/* p_login is empty for many daemons; fall back to the uid. */
		if (kp[i].p_login[0] != '\0') {
			strlcpy(p->user, kp[i].p_login, sizeof(p->user));
		} else {
			snprintf(p->user, sizeof(p->user), "%u",
			    (unsigned)kp[i].p_ruid);
		}
		strlcpy(p->comm, kp[i].p_comm, sizeof(p->comm));
		if (p->comm[0] == '\0')
			strlcpy(p->comm, "<unknown>", sizeof(p->comm));

		count++;
	}

	qsort(out, count, sizeof(*out), by_cpu_desc);
	return count;
}

void
read_meminfo(struct meminfo *m)
{
	struct uvmexp_sysctl u;
	int mib[2];
	size_t len = sizeof(u);
	uint64_t ps;

	memset(m, 0, sizeof(*m));
	mib[0] = CTL_VM;
	mib[1] = VM_UVMEXP2;
	if (sysctl(mib, 2, &u, &len, NULL, 0) != 0)
		return;

	ps = (uint64_t)u.pagesize;
	m->total_bytes  = (uint64_t)u.npages * ps;
	m->free_bytes   = (uint64_t)u.free   * ps;
	m->active_bytes = (uint64_t)u.active * ps;
	m->wired_bytes  = (uint64_t)u.wired  * ps;
}

uint32_t
read_loadavg(void)
{
	struct loadavg la;
	int mib[2];
	size_t len = sizeof(la);

	mib[0] = CTL_VM;
	mib[1] = VM_LOADAVG;
	if (sysctl(mib, 2, &la, &len, NULL, 0) != 0 || la.fscale <= 0)
		return 0;
	return (uint32_t)divround((uint64_t)la.ldavg[0] * LOAD_SCALE,
	    (uint64_t)la.fscale);
}

long
read_uptime(void)
{
	struct timeval bt;
	int mib[2];
	size_t len = sizeof(bt);

	mib[0] = CTL_KERN;
	mib[1] = KERN_BOOTTIME;
	if (sysctl(mib, 2, &bt, &len, NULL, 0) != 0 || bt.tv_sec == 0)
		return 0;
	return (long)(time(NULL) - bt.tv_sec);
}

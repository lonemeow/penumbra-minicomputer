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

int
read_procs(struct procinfo *out, int max)
{
	int mib[6];
	size_t len = 0, pgsz;
	struct kinfo_proc2 *kp;
	unsigned int i, n;
	int count = 0;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC2;
	mib[2] = KERN_PROC_ALL;
	mib[3] = 0;
	mib[4] = (int)sizeof(struct kinfo_proc2);
	mib[5] = 0;

	/* Sizing pass.  mib[5]=0 asks "how many bytes?". */
	if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0)
		return 0;

	/* Slack for procs forked between the two calls. */
	len += sizeof(struct kinfo_proc2) * 8;
	kp = malloc(len);
	if (kp == NULL)
		return 0;

	mib[5] = (int)(len / sizeof(struct kinfo_proc2));
	if (sysctl(mib, 6, kp, &len, NULL, 0) != 0) {
		free(kp);
		return 0;
	}
	n = len / sizeof(struct kinfo_proc2);

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

	free(kp);
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

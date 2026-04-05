/*	$NetBSD$	*/

#ifndef _PENUMBRA_CPU_H_
#define _PENUMBRA_CPU_H_

#if defined(_KERNEL) || defined(_KMEMUSER)

#include <sys/cpu_data.h>
#include <machine/frame.h>
#include <machine/intr.h>

struct cpu_info {
	struct cpu_data ci_data;		/* MI per-CPU data */
	struct lwp	*ci_curlwp;		/* current LWP */
	struct lwp	*ci_onproc;		/* current user LWP / kthread */
	cpuid_t		ci_cpuid;
	struct device	*ci_dev;
	int		ci_want_resched;
	int		ci_idepth;		/* interrupt depth */
	int		ci_cpl;			/* current priority level */
	volatile int	ci_mtx_count;
	volatile int	ci_mtx_oldspl;
};

extern struct cpu_info cpu_info_store;
#define	curcpu()		(&cpu_info_store)
#define	cpu_number()		0		/* uniprocessor */

void	cpu_startup(void);
void	cpu_reboot(int, char *);
void	cpu_need_resched(struct cpu_info *, struct lwp *, int);
void	cpu_need_proftick(struct lwp *);
void	cpu_signotify(struct lwp *);
void	cpu_lwp_free(struct lwp *, int);
void	cpu_lwp_free2(struct lwp *);
int	cpu_lwp_setprivate(struct lwp *, void *);
void	cpu_idle(void);

#endif /* _KERNEL || _KMEMUSER */

/*
 * CTL_MACHDEP definitions (sysctl).
 */
#define CPU_MAXID		1

#endif /* _PENUMBRA_CPU_H_ */

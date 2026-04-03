/*	$NetBSD$	*/

#ifndef _PENUMBRA_SIGNAL_H_
#define _PENUMBRA_SIGNAL_H_

#include <sys/featuretest.h>

typedef int sig_atomic_t;

#if defined(_NETBSD_SOURCE)
#include <sys/sigtypes.h>

struct sigcontext {
	int	sc_onstack;
	int	__sc_mask13;
	int	sc_pc;
	int	sc_regs[16];	/* R0-R15 */
	int	sc_sr;		/* status register */
	sigset_t sc_mask;
};
#endif /* _NETBSD_SOURCE */

#endif /* _PENUMBRA_SIGNAL_H_ */

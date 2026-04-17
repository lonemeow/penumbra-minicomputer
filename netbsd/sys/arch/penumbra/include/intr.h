/*	$NetBSD$	*/

#ifndef _PENUMBRA_INTR_H_
#define _PENUMBRA_INTR_H_

/*
 * Interrupt priority levels.
 * These map to the SR.I (interrupt enable) bit and software
 * masking — Penumbra has a single IRQ line with an external
 * interrupt controller (planned).
 */

#define IPL_NONE	0	/* nothing */
#define IPL_SOFTCLOCK	1	/* software clock interrupt */
#define IPL_SOFTBIO	2	/* software block I/O */
#define IPL_SOFTNET	3	/* software network */
#define IPL_SOFTSERIAL	4	/* software serial */
#define IPL_VM		5	/* memory allocation, buffer cache */
#define IPL_SCHED	6	/* scheduler, clock */
#define IPL_DDB		7	/* kernel debugger */
#define IPL_HIGH	8	/* everything */

#define NIPL		9

/* IST_* interrupt sharing types */
#define IST_NONE	0	/* none */
#define IST_PULSE	1	/* pulsed */
#define IST_EDGE	2	/* edge-triggered */
#define IST_LEVEL	3	/* level-triggered */

#ifdef _KERNEL

#include <machine/psl.h>

typedef unsigned char ipl_t;
typedef struct {
	ipl_t	_spl;
} ipl_cookie_t;

static inline ipl_cookie_t
makeiplcookie(ipl_t ipl)
{
	return (ipl_cookie_t){._spl = ipl};
}

static inline int
splraiseipl(ipl_cookie_t icookie)
{
	uint32_t sr;
	int old;

	__asm __volatile("RDSPR %0, sr" : "=r"(sr));
	old = (sr & PSL_I) ? IPL_NONE : IPL_HIGH;
	if (icookie._spl > IPL_NONE)
		__asm __volatile("DI");
	return old;
}

int	splraise(int);
void	splx(int);
void	spl0(void);
int	splhigh(void);
int	splsoftclock(void);
int	splsoftbio(void);
int	splsoftnet(void);
int	splsoftserial(void);
int	splvm(void);
int	splsched(void);
int	splddb(void);

void	intr_init(void);
void	intr_dispatch(void);
void   *intr_establish(int irq, int ipl, int (*handler)(void *), void *arg);
void   *intr_establish_xname(int irq, int ipl, int (*handler)(void *),
	    void *arg, const char *xname);
void	intr_disestablish(void *cookie);

#endif /* _KERNEL */

#endif /* _PENUMBRA_INTR_H_ */

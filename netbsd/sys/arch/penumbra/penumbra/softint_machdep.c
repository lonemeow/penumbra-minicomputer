/*	$NetBSD$	*/

/*
 * Penumbra fast soft interrupts.
 *
 * A soft interrupt runs on its own LWP's stack, borrowing the context of
 * the LWP it interrupts rather than going through the scheduler
 * (softint_fast_dispatch in locore.S).  There is no software interrupt
 * line to raise, so delivery happens wherever the CPU returns to
 * IPL_NONE: splx(), spl0(), and the end of trap().
 *
 * SR.I cannot express the soft levels, so the LWP carries one: a softint
 * LWP masks its own level and every lower one (md_softint_mask, zero for
 * every other LWP), and delivery skips what curlwp masks.  Being
 * per-LWP, the level survives a handler that blocks and resumes as an
 * ordinary thread, and nothing can desync it from exception entry.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bitops.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/lwp.h>

#include <machine/psl.h>

/*
 * Priority rank: a level's IPL above IPL_SOFTCLOCK, since the SOFTINT_*
 * numbering does not follow priority.  Indexes ci_softints and
 * ci_softlwps[], so the levels at or below a rank are a contiguous mask.
 */
static const u_int softint_rank[SOFTINT_COUNT] = {
	[SOFTINT_CLOCK]  = IPL_SOFTCLOCK  - IPL_SOFTCLOCK,
	[SOFTINT_BIO]    = IPL_SOFTBIO    - IPL_SOFTCLOCK,
	[SOFTINT_NET]    = IPL_SOFTNET    - IPL_SOFTCLOCK,
	[SOFTINT_SERIAL] = IPL_SOFTSERIAL - IPL_SOFTCLOCK,
};
CTASSERT(IPL_SOFTSERIAL - IPL_SOFTCLOCK == SOFTINT_COUNT - 1);

void	softint_fast_dispatch(struct lwp *, int);	/* locore.S */

void
softint_init_md(lwp_t *l, u_int level, uintptr_t *machdep)
{
	struct cpu_info * const ci = l->l_cpu;
	const u_int rank = softint_rank[level];

	KASSERT(ci->ci_softlwps[rank] == NULL);
	ci->ci_softlwps[rank] = l;
	l->l_md.md_softint_mask = (2U << rank) - 1;
	*machdep = 1U << rank;
}

/* Called at IPL_HIGH; the next return to IPL_NONE delivers it. */
void
softint_trigger(uintptr_t machdep)
{

	curcpu()->ci_softints |= machdep;
}

/*
 * Run the pending soft interrupts that curlwp does not mask, highest
 * priority first.  Called and returns with interrupts disabled.
 */
void
softint_deliver(void)
{
	struct cpu_info * const ci = curcpu();
	const u_int masked = curlwp->l_md.md_softint_mask;
	u_int pending;

#ifdef DIAGNOSTIC
	uint32_t sr;

	__asm __volatile("RDSPR %0, sr" : "=r"(sr));
	KASSERT((sr & PSL_I) == 0);
#endif
	KASSERT(ci->ci_idepth == 0);
	KASSERT(ci->ci_mtx_count == 0);

	while ((pending = ci->ci_softints & ~masked) != 0) {
		const u_int rank = fls32(pending) - 1;

		ci->ci_softints &= ~(1U << rank);
		/* Handlers run unmasked; their level is the LWP's. */
		softint_fast_dispatch(ci->ci_softlwps[rank], IPL_NONE);
		KASSERT(ci->ci_mtx_count == 0);
	}
}

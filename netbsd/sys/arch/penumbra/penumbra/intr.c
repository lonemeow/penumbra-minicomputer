/*	$NetBSD$	*/

/*
 * Penumbra interrupt dispatch.
 *
 * Penumbra has a single shared IRQ line (wired-OR of every device's
 * open-drain /IRQ output) and no interrupt controller.  The kernel
 * identifies the source by polling each registered device's IRQ
 * status register.  See doc/TODO.md "Interrupt Model".
 *
 * Drivers register a handler via intr_establish() during attach.
 * On EXC_EXT_IRQ, trap.c calls intr_dispatch(), which walks every
 * handler and calls it.  A handler returns 1 if it saw pending
 * work for its device (and serviced it), 0 otherwise.
 *
 * Because the IRQ line is wire-OR'd, intr_dispatch() must call
 * every handler even after one claims — any other device that is
 * also asserting would otherwise be stranded, and the line stays
 * high, producing an interrupt storm on rte.
 *
 * The "irq" parameter to intr_establish() is ignored on Penumbra
 * (there is no IRQ number); it is accepted for MI driver source
 * compatibility.  The "ipl" parameter is stored for diagnostics
 * only — all handlers run with SR.I=0 (see intr.h binary-IPL note).
 *
 * Per-handler statistics are exposed via evcnt(9) under the group
 * "shared irq" — visible through vmstat -i and sysctl
 * kern.intr.intrs.  A separate "shared irq spurious" counter
 * accumulates IRQs that no handler claimed.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/kmem.h>
#include <sys/evcnt.h>

#include <machine/intr.h>

#define INTR_GROUP	"shared irq"

struct intrhand {
	LIST_ENTRY(intrhand)	 ih_link;
	int			(*ih_fun)(void *);
	void			*ih_arg;
	/*
	 * ih_ipl is currently stored but unused in dispatch.  Penumbra
	 * has binary IPL (SR.I is one bit), so every handler runs at
	 * effective IPL_HIGH regardless of the requested level.  Kept
	 * for future soft-IPL masking or intrctl(8)-style diagnostics,
	 * and so we don't silently discard the MI intr_establish() arg.
	 */
	int			 ih_ipl;
	struct evcnt		 ih_evcnt;	/* claim count (via evcnt(9)) */
	char			 ih_xname[16];
};

static LIST_HEAD(, intrhand) intr_handlers =
    LIST_HEAD_INITIALIZER(intr_handlers);

static struct evcnt intr_spurious_evcnt;

void
intr_init(void)
{
	/*
	 * LIST_HEAD is statically initialised and interrupts are
	 * already disabled by hardware at boot.  Only the spurious
	 * counter needs explicit attach.
	 */
	evcnt_attach_dynamic(&intr_spurious_evcnt, EVCNT_TYPE_INTR,
	    NULL, INTR_GROUP, "spurious");
}

void *
intr_establish_xname(int irq, int ipl, int (*handler)(void *), void *arg,
    const char *xname)
{
	struct intrhand *ih;
	int s;

	(void)irq;	/* Penumbra has a single shared IRQ line */

	ih = kmem_alloc(sizeof(*ih), KM_SLEEP);
	ih->ih_fun = handler;
	ih->ih_arg = arg;
	ih->ih_ipl = ipl;
	strlcpy(ih->ih_xname, xname ? xname : "unknown", sizeof(ih->ih_xname));

	/*
	 * Attach evcnt before linking — the evcnt name points into
	 * ih_xname, so the evcnt and the list entry must share
	 * lifetime anyway, but attaching first means a dispatch
	 * mid-registration finds a fully-initialised counter.
	 */
	evcnt_attach_dynamic(&ih->ih_evcnt, EVCNT_TYPE_INTR, NULL,
	    INTR_GROUP, ih->ih_xname);

	s = splhigh();
	LIST_INSERT_HEAD(&intr_handlers, ih, ih_link);
	splx(s);

	return ih;
}

void *
intr_establish(int irq, int ipl, int (*handler)(void *), void *arg)
{
	return intr_establish_xname(irq, ipl, handler, arg, "unknown");
}

void
intr_disestablish(void *cookie)
{
	struct intrhand *ih = cookie;
	int s;

	s = splhigh();
	LIST_REMOVE(ih, ih_link);
	splx(s);

	evcnt_detach(&ih->ih_evcnt);
	kmem_free(ih, sizeof(*ih));
}

/*
 * intr_dispatch — called from trap.c EXC_EXT_IRQ with SR.I already
 * cleared by hardware (interrupts disabled for the duration of the
 * handler run).  ci_idepth bracketing is done by the caller.
 *
 * Returns nothing — any "I couldn't service this" condition is
 * handler-local and counted via intr_spurious_evcnt.
 */
void
intr_dispatch(void)
{
	struct intrhand *ih;
	int claimed = 0;

	LIST_FOREACH(ih, &intr_handlers, ih_link) {
		int res = ih->ih_fun(ih->ih_arg);
		if (res) {
			claimed = 1;
			ih->ih_evcnt.ev_count++;
		}
	}

	if (!claimed)
		intr_spurious_evcnt.ev_count++;
}

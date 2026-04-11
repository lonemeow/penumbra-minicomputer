/*	$NetBSD$	*/

/*
 * com_pbbus — MI com(4) attachment for Penumbra Bus.
 *
 * Matches ACFG_CLASS_UART devices discovered by pbbus during autoconfig.
 * The UART is 16450-compatible with word-strided registers (reg_shift=2,
 * 32-bit wide accesses).
 *
 * No interrupt support yet — uses polled I/O via the MI com driver's
 * built-in poll callout (sc_poll_ticks).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/termios.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

#include <dev/ic/comreg.h>
#include <dev/ic/comvar.h>

/*
 * Standard 16450 crystal frequency.  Irrelevant for simulation
 * (the ISS ignores baud rate divisors) but the MI com driver
 * needs a frequency to compute divisors during initialization.
 * 1843200 / (16 * 115200) = 1, which is a valid divisor.
 */
#define COM_PBBUS_FREQ	1843200

/* Register stride: our UART has 32-bit-wide, word-strided registers */
#define COM_PBBUS_REGSHIFT	2
#define COM_PBBUS_REGWIDTH	4

static int	com_pbbus_match(device_t, cfdata_t, void *);
static void	com_pbbus_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(com_pbbus, sizeof(struct com_softc),
    com_pbbus_match, com_pbbus_attach, NULL, NULL);

static int
com_pbbus_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_UART;
}

static void
com_pbbus_attach(device_t parent, device_t self, void *aux)
{
	struct com_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	bus_space_handle_t bsh;
	int error;

	sc->sc_dev = self;

	error = bus_space_map(pa->pb_iot, pa->pb_addr, pa->pb_size,
	    0, &bsh);
	if (error) {
		aprint_error(": can't map registers: %d\n", error);
		return;
	}

	com_init_regs_stride_width(&sc->sc_regs, pa->pb_iot, bsh,
	    pa->pb_addr, COM_PBBUS_REGSHIFT, COM_PBBUS_REGWIDTH);

	sc->sc_frequency = COM_PBBUS_FREQ;

	/*
	 * No interrupt handler wired — use polled I/O via the MI
	 * com driver's built-in poll callout.
	 *
	 * COM_HW_NOIEN suppresses MCR_IENABLE (OUT2), which gates
	 * the UART's IRQ output (see ISS: irq = !IIR_NOPEND && MCR[3]).
	 * This prevents actual IRQ delivery to the CPU.
	 *
	 * IER is still set by comopen() (IER_ERXRDY | IER_ERLS),
	 * so IIR correctly reports pending RX/TX data.  comintr()
	 * checks IIR first — without IER enabled, it bails on
	 * IIR_NOPEND and never processes input.
	 */
	SET(sc->sc_hwflags, COM_HW_NOIEN);
	sc->sc_poll_ticks = 1;

	/*
	 * Mark as console so com_attach_subr() sets cn_dev on
	 * cn_tab (the early boot console from startup.c).
	 * This lets MI cnopen() redirect /dev/console to com's
	 * cdevsw for userland I/O.
	 *
	 * We intentionally skip comcnattach1() here — it would
	 * call cominit() → bus_space_map() creating a redundant
	 * mapping (startup.c already mapped the UART via
	 * pmap_map_device).  The early console stays active for
	 * kernel printf; the MI com tty layer handles userland.
	 */
	SET(sc->sc_hwflags, COM_HW_CONSOLE);

	com_attach_subr(sc);
}

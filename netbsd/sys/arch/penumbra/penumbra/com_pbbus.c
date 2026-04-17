/*	$NetBSD$	*/

/*
 * com_pbbus — MI com(4) attachment for Penumbra Bus.
 *
 * Matches ACFG_CLASS_UART devices discovered by pbbus during autoconfig.
 * The UART is 16450-compatible with word-strided registers (reg_shift=2,
 * 32-bit wide accesses).
 *
 * IRQ-driven: comintr() registers via intr_establish_xname() on the
 * shared /IRQ line.  See penumbra/intr.c for the dispatcher.
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
#include <machine/intr.h>
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
	 * Fully IRQ-driven.  The MI com driver asserts MCR_IENABLE
	 * (OUT2) itself, which gates the UART's IRQ output through
	 * to the shared /IRQ line.  Our handler shows up in
	 * vmstat -i under "shared irq com0" via the evcnt attached
	 * inside intr_establish_xname().
	 */
	intr_establish_xname(0, IPL_SERIAL, comintr, sc, device_xname(self));

	/*
	 * Register as the system console via comcnattach1().
	 * This sets up cn_tab (MI com console vtable),
	 * comcons_info (rate/cflag for comopen defaults), and
	 * cn_init_magic (console magic sequence — null without
	 * this causes a panic on first RX character).
	 *
	 * cominit() inside comcnattach1 creates a second
	 * bus_space mapping of the same UART PA — harmless
	 * (uncached MMIO, two VAs for one PA is fine).
	 *
	 * com_attach_subr() then auto-detects COM_HW_CONSOLE
	 * by matching cr_iot + cr_iobase against comcons_info.
	 */
	comcnattach1(&sc->sc_regs, 115200, COM_PBBUS_FREQ,
	    COM_TYPE_NORMAL, (TTYDEF_CFLAG & ~(CSIZE | PARENB)) | CS8);

	com_attach_subr(sc);
}

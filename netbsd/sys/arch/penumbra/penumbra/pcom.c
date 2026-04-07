/*	$NetBSD$	*/

/*
 * pcom — Penumbra console UART driver.
 *
 * 16450-compatible UART with word-strided registers (reg_shift=2).
 * Attaches via pbbus when class == PBBUS_CLASS_UART.
 *
 * Takes over cn_tab from the early boot console in startup.c.
 * The early console's pmap_map_device() mapping is abandoned
 * (harmless — it's a wired kernel page).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/tty.h>
#include <sys/conf.h>

#include <dev/cons.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

/* 16450 register offsets (word-strided: byte offset = reg * 4) */
#define PCOM_RBR	0x00	/* Receive Buffer (read) */
#define PCOM_THR	0x00	/* Transmit Holding (write) */
#define PCOM_IER	0x04	/* Interrupt Enable */
#define PCOM_IIR	0x08	/* Interrupt ID (read) */
#define PCOM_LCR	0x0C	/* Line Control */
#define PCOM_MCR	0x10	/* Modem Control */
#define PCOM_LSR	0x14	/* Line Status */
#define PCOM_MSR	0x18	/* Modem Status */
#define PCOM_SCR	0x1C	/* Scratch */

#define LSR_DR		0x01	/* Data Ready */
#define LSR_THRE	0x20	/* TX Holding Register Empty */

struct pcom_softc {
	device_t	sc_dev;
	bus_space_tag_t	sc_iot;
	bus_space_handle_t sc_ioh;
};

/* There is only one console UART */
static struct pcom_softc *pcom_console_sc;

static int	pcom_match(device_t, cfdata_t, void *);
static void	pcom_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(pcom, sizeof(struct pcom_softc),
    pcom_match, pcom_attach, NULL, NULL);

/* Console operations */
static int	pcom_cngetc(dev_t);
static void	pcom_cnputc(dev_t, int);
static void	pcom_cnpollc(dev_t, int);

static struct consdev pcom_consdev = {
	.cn_getc = pcom_cngetc,
	.cn_putc = pcom_cnputc,
	.cn_pollc = pcom_cnpollc,
	.cn_dev = NODEV,
	.cn_pri = CN_REMOTE,
};

static int
pcom_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_UART;
}

static void
pcom_attach(device_t parent, device_t self, void *aux)
{
	struct pcom_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map registers: %d\n", error);
		return;
	}

	aprint_normal(": NS16450 UART (console)\n");

	/* Take over as console */
	pcom_console_sc = sc;
	cn_tab = &pcom_consdev;
}

static int
pcom_cngetc(dev_t dev)
{
	struct pcom_softc *sc = pcom_console_sc;

	if (sc == NULL)
		return -1;

	while (!(bus_space_read_4(sc->sc_iot, sc->sc_ioh, PCOM_LSR) & LSR_DR))
		;
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, PCOM_RBR) & 0xFF;
}

static void
pcom_cnputc(dev_t dev, int c)
{
	struct pcom_softc *sc = pcom_console_sc;

	if (sc == NULL)
		return;

	while (!(bus_space_read_4(sc->sc_iot, sc->sc_ioh, PCOM_LSR) & LSR_THRE))
		;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, PCOM_THR, (uint32_t)c);
}

static void
pcom_cnpollc(dev_t dev, int on)
{
	/* nothing */
}

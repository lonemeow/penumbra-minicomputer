/*	$NetBSD$	*/

/*
 * Penumbra display adapter (CLASS_DISPLAY) wsdisplay driver.
 *
 * The device is a character-cell console: a CELLS aperture of 16-bit
 * {attr, glyph} cells in word-strided slots, a hardware cursor, and
 * the CGA/ANSI 16-color attribute model (doc/system/devices/display.md).
 * That is exactly the shape wsdisplay's character emulops want, so the
 * driver is a thin translation: each emulop writes cells through
 * bus_space, one 32-bit word per cell.
 *
 * A RAM shadow of the cell grid backs the copy operations: device
 * reads are uncached MMIO and would dominate scrolling, so copyrows /
 * copycols move cells in the shadow and replay only the stores to the
 * device.  The shadow starts blank while the screen may still show
 * the power-up splash; the emulation screen begins with a full clear,
 * which brings the two in sync before the first copy ever runs.
 *
 * The cursor is the device's own (CURSOR register + CTRL.CURSOR_EN),
 * so the driver never draws one.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>
#include <dev/wsfont/wsfont.h>

/* Register offsets (doc/system/devices/display.md). */
#define	PD_CAP		0x000
#define	PD_INFO		0x004
#define	PD_CTRL		0x008
#define	PD_CURSOR	0x00C
#define	PD_CELLS	0x1000

#define	PD_CAP_VERSION(c)	((c) & 0xFF)
#define	PD_CAP_COLOR		__BIT(8)
#define	PD_CAP_EXTGLYPHS	__BIT(10)
#define	PD_CTRL_ENABLE		__BIT(0)
#define	PD_CTRL_CURSOR_EN	__BIT(1)

struct pdisplay_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	int			sc_cols;
	int			sc_rows;
	uint32_t		sc_ctrl;	/* CTRL shadow (MMIO reads cost) */
	uint16_t		*sc_shadow;	/* cell grid shadow */
	int			sc_nscreens;
	struct wsscreen_descr	sc_screen;
};

#define	PD_RD(sc, reg)		bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (reg))
#define	PD_WR(sc, reg, val)	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (reg), (val))
#define	PD_CELL_WR(sc, idx, cell) \
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, PD_CELLS + (idx) * 4, (cell))

static int	pdisplay_match(device_t, cfdata_t, void *);
static void	pdisplay_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(pdisplay, sizeof(struct pdisplay_softc),
    pdisplay_match, pdisplay_attach, NULL, NULL);

/* ── wsdisplay emulops ────────────────────────────────────────── */

static int	pdisplay_mapchar(void *, int, u_int *);
static void	pdisplay_cursor(void *, int, int, int);
static void	pdisplay_putchar(void *, int, int, u_int, long);
static void	pdisplay_copycols(void *, int, int, int, int);
static void	pdisplay_erasecols(void *, int, int, int, long);
static void	pdisplay_copyrows(void *, int, int, int);
static void	pdisplay_eraserows(void *, int, int, long);
static int	pdisplay_allocattr(void *, int, int, int, long *);

static const struct wsdisplay_emulops pdisplay_emulops = {
	.cursor		= pdisplay_cursor,
	.mapchar	= pdisplay_mapchar,
	.putchar	= pdisplay_putchar,
	.copycols	= pdisplay_copycols,
	.erasecols	= pdisplay_erasecols,
	.copyrows	= pdisplay_copyrows,
	.eraserows	= pdisplay_eraserows,
	.allocattr	= pdisplay_allocattr,
};

/* ── wsdisplay accessops ──────────────────────────────────────── */

static int	pdisplay_ioctl(void *, void *, u_long, void *, int, struct lwp *);
static paddr_t	pdisplay_mmap(void *, void *, off_t, int);
static int	pdisplay_alloc_screen(void *, const struct wsscreen_descr *,
		    void **, int *, int *, long *);
static void	pdisplay_free_screen(void *, void *);
static int	pdisplay_show_screen(void *, void *, int,
		    void (*)(void *, int, int), void *);

static const struct wsdisplay_accessops pdisplay_accessops = {
	.ioctl		= pdisplay_ioctl,
	.mmap		= pdisplay_mmap,
	.alloc_screen	= pdisplay_alloc_screen,
	.free_screen	= pdisplay_free_screen,
	.show_screen	= pdisplay_show_screen,
};

/* ── Autoconf ─────────────────────────────────────────────────── */

static int
pdisplay_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_DISPLAY;
}

static void
pdisplay_attach(device_t parent, device_t self, void *aux)
{
	struct pdisplay_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	struct wsemuldisplaydev_attach_args aa;
	uint32_t cap, info;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map display registers: %d\n", error);
		return;
	}

	cap = PD_RD(sc, PD_CAP);
	info = PD_RD(sc, PD_INFO);
	sc->sc_cols = info & 0xFFFF;
	sc->sc_rows = (info >> 16) & 0xFFFF;
	sc->sc_shadow = kmem_zalloc(sc->sc_cols * sc->sc_rows *
	    sizeof(uint16_t), KM_SLEEP);

	/*
	 * Known control state: picture on, cursor off until the
	 * emulator owns a position.  The splash (if the board
	 * preloads one) stays on screen until the first clear.
	 */
	sc->sc_ctrl = PD_CTRL_ENABLE;
	PD_WR(sc, PD_CTRL, sc->sc_ctrl);

	aprint_naive("\n");
	aprint_normal(": Penumbra display adapter (v%u, %ux%u%s%s)\n",
	    PD_CAP_VERSION(cap), sc->sc_cols, sc->sc_rows,
	    (cap & PD_CAP_COLOR) ? ", color" : "",
	    (cap & PD_CAP_EXTGLYPHS) ? ", cp437" : "");

	/*
	 * A monochrome device (CAP.COLOR clear) would advertise
	 * WSSCREEN_REVERSE instead and swap nibbles in allocattr.
	 */
	sc->sc_screen = (struct wsscreen_descr){
		.name = "std",
		.ncols = sc->sc_cols,
		.nrows = sc->sc_rows,
		.textops = &pdisplay_emulops,
		.fontwidth = 8,
		.fontheight = 16,
		.capabilities = WSSCREEN_WSCOLORS | WSSCREEN_HILIT,
	};

	static const struct wsscreen_descr *scrlist[1];
	static struct wsscreen_list screenlist;
	scrlist[0] = &sc->sc_screen;
	screenlist.nscreens = 1;
	screenlist.screens = scrlist;

	memset(&aa, 0, sizeof(aa));
	aa.console = false;
	aa.scrdata = &screenlist;
	aa.accessops = &pdisplay_accessops;
	aa.accesscookie = sc;
	config_found(self, &aa, wsemuldisplaydevprint, CFARGS_NONE);
}

/* ── Emulops ──────────────────────────────────────────────────── */

/*
 * wsfont_map_unichar() reads nothing but the encoding.  The mapping
 * belongs here rather than in putchar: the emulation resolves each
 * character set once through mapchar and passes glyph indices after.
 */
static struct wsdisplay_font pdisplay_fontdesc = {
	.encoding = WSDISPLAY_FONTENC_IBM,
};

static int
pdisplay_mapchar(void *cookie, int uni, u_int *index)
{
	int glyph;

	/* Glyph 0 is the table's "no equivalent" answer. */
	glyph = wsfont_map_unichar(&pdisplay_fontdesc, uni);
	if (glyph > 0 && glyph <= 0xFF) {
		*index = (u_int)glyph;
		return 5;
	}
	*index = '?';
	return 0;
}

static void
pdisplay_cursor(void *cookie, int on, int row, int col)
{
	struct pdisplay_softc *sc = cookie;

	if (on) {
		PD_WR(sc, PD_CURSOR, ((uint32_t)row << 16) | (uint32_t)col);
		sc->sc_ctrl |= PD_CTRL_CURSOR_EN;
	} else {
		sc->sc_ctrl &= ~PD_CTRL_CURSOR_EN;
	}
	PD_WR(sc, PD_CTRL, sc->sc_ctrl);
}

static void
pdisplay_putchar(void *cookie, int row, int col, u_int uc, long attr)
{
	struct pdisplay_softc *sc = cookie;
	int idx = row * sc->sc_cols + col;
	uint16_t cell = (uint16_t)((attr << 8) | (uc & 0xFF));

	sc->sc_shadow[idx] = cell;
	PD_CELL_WR(sc, idx, cell);
}

static void
pdisplay_copycols(void *cookie, int row, int srccol, int dstcol, int ncols)
{
	struct pdisplay_softc *sc = cookie;
	int base = row * sc->sc_cols;

	memmove(&sc->sc_shadow[base + dstcol], &sc->sc_shadow[base + srccol],
	    ncols * sizeof(uint16_t));
	for (int i = 0; i < ncols; i++)
		PD_CELL_WR(sc, base + dstcol + i, sc->sc_shadow[base + dstcol + i]);
}

static void
pdisplay_erasecols(void *cookie, int row, int startcol, int ncols, long attr)
{
	struct pdisplay_softc *sc = cookie;
	int base = row * sc->sc_cols;
	uint16_t cell = (uint16_t)((attr << 8) | ' ');

	for (int i = 0; i < ncols; i++) {
		sc->sc_shadow[base + startcol + i] = cell;
		PD_CELL_WR(sc, base + startcol + i, cell);
	}
}

static void
pdisplay_copyrows(void *cookie, int srcrow, int dstrow, int nrows)
{
	struct pdisplay_softc *sc = cookie;
	int n = nrows * sc->sc_cols;
	int src = srcrow * sc->sc_cols;
	int dst = dstrow * sc->sc_cols;

	memmove(&sc->sc_shadow[dst], &sc->sc_shadow[src],
	    n * sizeof(uint16_t));
	for (int i = 0; i < n; i++)
		PD_CELL_WR(sc, dst + i, sc->sc_shadow[dst + i]);
}

static void
pdisplay_eraserows(void *cookie, int row, int nrows, long attr)
{
	struct pdisplay_softc *sc = cookie;
	int start = row * sc->sc_cols;
	int n = nrows * sc->sc_cols;
	uint16_t cell = (uint16_t)((attr << 8) | ' ');

	for (int i = 0; i < n; i++) {
		sc->sc_shadow[start + i] = cell;
		PD_CELL_WR(sc, start + i, cell);
	}
}

static int
pdisplay_allocattr(void *cookie, int fg, int bg, int flags, long *attrp)
{

	if (flags & (WSATTR_UNDERLINE | WSATTR_BLINK | WSATTR_REVERSE))
		return EINVAL;

	if (!(flags & WSATTR_WSCOLORS)) {
		fg = WSCOL_WHITE;
		bg = WSCOL_BLACK;
	}
	if (flags & WSATTR_HILIT)
		fg += 8;

	*attrp = fg | (bg << 4);
	return 0;
}

/* ── Accessops ────────────────────────────────────────────────── */

static int
pdisplay_ioctl(void *v, void *vs, u_long cmd, void *data, int flag,
    struct lwp *l)
{

	switch (cmd) {
	case WSDISPLAYIO_GTYPE:
		*(u_int *)data = WSDISPLAY_TYPE_UNKNOWN;
		return 0;
	}
	return EPASSTHROUGH;
}

static paddr_t
pdisplay_mmap(void *v, void *vs, off_t off, int prot)
{

	/* WSDISPLAYIO_MODE_MAPPED cell access: a later refinement. */
	return -1;
}

static int
pdisplay_alloc_screen(void *v, const struct wsscreen_descr *type,
    void **cookiep, int *curxp, int *curyp, long *defattrp)
{
	struct pdisplay_softc *sc = v;

	if (sc->sc_nscreens > 0)
		return ENOMEM;
	sc->sc_nscreens++;

	*cookiep = sc;
	*curxp = *curyp = 0;
	return pdisplay_allocattr(sc, 0, 0, 0, defattrp);
}

static void
pdisplay_free_screen(void *v, void *cookie)
{
	struct pdisplay_softc *sc = v;

	sc->sc_nscreens--;
}

static int
pdisplay_show_screen(void *v, void *cookie, int waitok,
    void (*cb)(void *, int, int), void *cbarg)
{

	/* Single screen — nothing to switch. */
	return 0;
}

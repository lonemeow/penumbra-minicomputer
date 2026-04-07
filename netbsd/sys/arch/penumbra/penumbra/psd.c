/*	$NetBSD$	*/

/*
 * psd — Penumbra SD card block device driver.
 *
 * Attaches via pbbus for ACFG_CLASS_SD devices.  Talks SD-SPI
 * protocol through the Penumbra SPI controller's MMIO registers
 * (DATA, STATUS, CONTROL, CLKDIV at word-stride offsets).
 *
 * Polled I/O only — no interrupts, no DMA.  Sufficient for
 * initial root filesystem mount and basic operation.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/disklabel.h>
#include <sys/buf.h>
#include <sys/bufq.h>
#include <sys/conf.h>
#include <sys/proc.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

/* ── SPI register offsets (word-strided) ──────────────────────── */

#define SPI_DATA	0x00	/* R/W: TX/RX byte */
#define SPI_STATUS	0x04	/* R:   bit 0 = BUSY */
#define SPI_CONTROL	0x08	/* R/W: bit 0 = CS0 */
#define SPI_CLKDIV	0x0C	/* R/W: clock divider */

#define SPI_STATUS_BUSY	0x01

/* ── SD SPI protocol constants ────────────────────────────────── */

#define SD_R1_IDLE	0x01
#define SD_R1_NO_RESP	0xFF

#define SD_CMD0_RETRIES		20
#define SD_ACMD41_RETRIES	1500
#define SD_DATA_RETRIES		1000
#define SD_RESP_RETRIES		8

#define SD_SECTOR_SIZE		512

/* ── MBR constants ────────────────────────────────────────────── */

#define MBR_SIG_OFFSET		510
#define MBR_SIGNATURE		0xAA55
#define MBR_PART_OFFSET		446
#define MBR_PART_ENTRY_SIZE	16
#define MBR_MAX_PARTS		4

/*
 * Partition mapping:
 *   a = NetBSD root (not used yet)
 *   b = swap (not used yet)
 *   c = raw (whole device)
 *   d = unused
 *   e..h = MBR partitions 1..4
 */
#define PART_MBR_BASE	4	/* partition 'e' = MBR partition 1 */

struct psd_part {
	uint32_t	offset;		/* LBA start */
	uint32_t	size;		/* sector count */
	uint8_t		type;		/* MBR type byte */
};

/* ── Softc ────────────────────────────────────────────────────── */

struct psd_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	struct disk		sc_dk;
	struct psd_part		sc_parts[MBR_MAX_PARTS];
	int			sc_nparts;
	bool			sc_attached;
};

static int	psd_match(device_t, cfdata_t, void *);
static void	psd_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(psd, sizeof(struct psd_softc),
    psd_match, psd_attach, NULL, NULL);

extern struct cfdriver psd_cd;

/* Block/char device prototypes */
static dev_type_open(psdopen);
static dev_type_close(psdclose);
static dev_type_read(psdread);
static dev_type_write(psdwrite);
static dev_type_ioctl(psdioctl);
static dev_type_strategy(psdstrategy);
static dev_type_size(psdsize);

const struct bdevsw psd_bdevsw = {
	.d_open = psdopen,
	.d_close = psdclose,
	.d_strategy = psdstrategy,
	.d_ioctl = psdioctl,
	.d_dump = nodump,
	.d_psize = psdsize,
	.d_discard = nodiscard,
	.d_flag = D_DISK,
};

const struct cdevsw psd_cdevsw = {
	.d_open = psdopen,
	.d_close = psdclose,
	.d_read = psdread,
	.d_write = psdwrite,
	.d_ioctl = psdioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_DISK,
};

static const struct dkdriver psd_dkdriver = {
	.d_strategy = psdstrategy,
	.d_minphys = minphys,
};

/* ── SPI low-level (bus_space) ────────────────────────────────── */

static inline uint8_t
psd_spi_transfer(struct psd_softc *sc, uint8_t tx)
{

	bus_space_write_4(sc->sc_iot, sc->sc_ioh, SPI_DATA, tx);
	while (bus_space_read_4(sc->sc_iot, sc->sc_ioh, SPI_STATUS) &
	    SPI_STATUS_BUSY)
		;
	return (uint8_t)bus_space_read_4(sc->sc_iot, sc->sc_ioh, SPI_DATA);
}

static inline void
psd_spi_cs(struct psd_softc *sc, int assert)
{
	uint32_t ctl;

	ctl = bus_space_read_4(sc->sc_iot, sc->sc_ioh, SPI_CONTROL);
	if (assert)
		ctl &= ~0x01u;		/* CS0 active low */
	else
		ctl |= 0x01u;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, SPI_CONTROL, ctl);
}

static inline void
psd_spi_clkdiv(struct psd_softc *sc, uint32_t div)
{

	bus_space_write_4(sc->sc_iot, sc->sc_ioh, SPI_CLKDIV, div);
}

/* ── SD SPI protocol ──────────────────────────────────────────── */

static uint8_t
psd_sd_command(struct psd_softc *sc, uint8_t cmd, uint32_t arg)
{
	uint8_t r;
	int i;

	psd_spi_transfer(sc, 0x40 | cmd);
	psd_spi_transfer(sc, (uint8_t)(arg >> 24));
	psd_spi_transfer(sc, (uint8_t)(arg >> 16));
	psd_spi_transfer(sc, (uint8_t)(arg >> 8));
	psd_spi_transfer(sc, (uint8_t)(arg));

	/* CRC — only CMD0 and CMD8 need valid CRC in SPI mode */
	if (cmd == 0)
		psd_spi_transfer(sc, 0x95);
	else if (cmd == 8)
		psd_spi_transfer(sc, 0x87);
	else
		psd_spi_transfer(sc, 0xFF);

	for (i = 0; i < SD_RESP_RETRIES; i++) {
		r = psd_spi_transfer(sc, 0xFF);
		if (!(r & 0x80))
			return r;
	}
	return r;
}

static uint32_t
psd_sd_read_r32(struct psd_softc *sc)
{
	uint32_t val;

	val  = (uint32_t)psd_spi_transfer(sc, 0xFF) << 24;
	val |= (uint32_t)psd_spi_transfer(sc, 0xFF) << 16;
	val |= (uint32_t)psd_spi_transfer(sc, 0xFF) << 8;
	val |= (uint32_t)psd_spi_transfer(sc, 0xFF);
	return val;
}

static int
psd_sd_init(struct psd_softc *sc)
{
	uint8_t r1;
	uint32_t r7, ocr;
	int i;

	/* Slow clock for init (<=400 kHz) */
	psd_spi_clkdiv(sc, 0xFF);

	/* 80+ clock cycles with CS deasserted */
	psd_spi_cs(sc, 0);
	for (i = 0; i < 20; i++)
		psd_spi_transfer(sc, 0xFF);

	psd_spi_cs(sc, 1);

	/* CMD0 — go idle */
	for (i = 0; i < SD_CMD0_RETRIES; i++) {
		r1 = psd_sd_command(sc, 0, 0);
		if (r1 == SD_R1_IDLE)
			break;
	}
	if (r1 != SD_R1_IDLE)
		return EIO;

	/* CMD8 — send interface condition */
	r1 = psd_sd_command(sc, 8, 0x1AA);
	if (r1 != SD_R1_IDLE)
		return EIO;
	r7 = psd_sd_read_r32(sc);
	if ((r7 & 0xFFF) != 0x1AA)
		return EIO;

	/* CMD55 + ACMD41 — wait for card ready */
	for (i = 0; i < SD_ACMD41_RETRIES; i++) {
		r1 = psd_sd_command(sc, 55, 0);
		if (r1 != SD_R1_IDLE)
			return EIO;
		r1 = psd_sd_command(sc, 41, 0x40000000);
		if (r1 == 0x00)
			break;
		if (r1 != SD_R1_IDLE)
			return EIO;
	}
	if (r1 != 0x00)
		return ETIMEDOUT;

	/* CMD58 — read OCR, verify SDHC */
	r1 = psd_sd_command(sc, 58, 0);
	ocr = psd_sd_read_r32(sc);
	if (!(ocr & 0x40000000))
		return ENODEV;

	/* Fast clock for data */
	psd_spi_clkdiv(sc, 0);
	return 0;
}

static void
psd_sd_deinit(struct psd_softc *sc)
{

	psd_spi_cs(sc, 0);
	psd_spi_clkdiv(sc, 0xFF);
}

static int
psd_sd_read_sector(struct psd_softc *sc, uint32_t lba, uint8_t *buf)
{
	uint8_t r1, tok;
	int i;

	r1 = psd_sd_command(sc, 17, lba);
	if (r1 != 0x00)
		return EIO;

	/* Wait for data token (0xFE) */
	for (i = 0; i < SD_DATA_RETRIES; i++) {
		tok = psd_spi_transfer(sc, 0xFF);
		if (tok == 0xFE)
			break;
	}
	if (tok != 0xFE)
		return EIO;

	for (i = 0; i < SD_SECTOR_SIZE; i++)
		buf[i] = psd_spi_transfer(sc, 0xFF);

	/* Discard CRC16 */
	psd_spi_transfer(sc, 0xFF);
	psd_spi_transfer(sc, 0xFF);

	return 0;
}

/* ── MBR partition parsing ─────────────────────────────────────── */

static inline uint16_t
psd_le16(const uint8_t *p)
{

	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t
psd_le32(const uint8_t *p)
{

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
psd_read_mbr(struct psd_softc *sc)
{
	uint8_t mbr[SD_SECTOR_SIZE];
	int i;

	sc->sc_nparts = 0;

	if (psd_sd_read_sector(sc, 0, mbr) != 0) {
		aprint_error_dev(sc->sc_dev, "can't read MBR\n");
		return;
	}

	if (psd_le16(mbr + MBR_SIG_OFFSET) != MBR_SIGNATURE) {
		aprint_normal_dev(sc->sc_dev, "no MBR signature\n");
		return;
	}

	for (i = 0; i < MBR_MAX_PARTS; i++) {
		const uint8_t *e = mbr + MBR_PART_OFFSET +
		    i * MBR_PART_ENTRY_SIZE;
		sc->sc_parts[i].type = e[4];
		sc->sc_parts[i].offset = psd_le32(e + 8);
		sc->sc_parts[i].size = psd_le32(e + 12);
		if (sc->sc_parts[i].type != 0)
			sc->sc_nparts = i + 1;
	}
}

/* ── Autoconf ─────────────────────────────────────────────────── */

static int
psd_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_SD;
}

static void
psd_attach(device_t parent, device_t self, void *aux)
{
	struct psd_softc *sc = device_private(self);
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

	/* Init card */
	error = psd_sd_init(sc);
	if (error) {
		aprint_error(": SD init failed: %d\n", error);
		psd_sd_deinit(sc);
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, pa->pb_size);
		return;
	}

	aprint_normal(": SD card (SDHC)");

	/* Read MBR to discover partitions */
	psd_read_mbr(sc);
	if (sc->sc_nparts > 0)
		aprint_normal(", %d partition%s", sc->sc_nparts,
		    sc->sc_nparts > 1 ? "s" : "");
	aprint_normal("\n");

	disk_init(&sc->sc_dk, device_xname(self), &psd_dkdriver);
	disk_attach(&sc->sc_dk);
	sc->sc_attached = true;
}

/* ── Block device operations ──────────────────────────────────── */

static struct psd_softc *
psd_lookup(dev_t dev)
{
	int unit = DISKUNIT(dev);

	return device_lookup_private(&psd_cd, unit);
}

/*
 * Translate a partition index to an MBR partition offset.
 * Returns 0 for the raw partition (c) or if partition is not in MBR range.
 * Returns -1 for invalid/empty partitions in the MBR range.
 */
static int
psd_part_offset(struct psd_softc *sc, int part, uint32_t *offsetp)
{

	if (part == RAW_PART) {
		*offsetp = 0;
		return 0;
	}

	/* MBR partitions: e=0, f=1, g=2, h=3 */
	if (part >= PART_MBR_BASE && part < PART_MBR_BASE + MBR_MAX_PARTS) {
		int mbr_idx = part - PART_MBR_BASE;
		if (sc->sc_parts[mbr_idx].type == 0)
			return -1;	/* empty */
		*offsetp = sc->sc_parts[mbr_idx].offset;
		return 0;
	}

	return -1;	/* no mapping */
}

static int
psdopen(dev_t dev, int flag, int fmt, struct lwp *l)
{
	struct psd_softc *sc;
	int part = DISKPART(dev);
	uint32_t offset;

	sc = psd_lookup(dev);
	if (sc == NULL || !sc->sc_attached)
		return ENXIO;

	if (psd_part_offset(sc, part, &offset) != 0)
		return ENXIO;

	return 0;
}

static int
psdclose(dev_t dev, int flag, int fmt, struct lwp *l)
{

	return 0;
}

static void
psdstrategy(struct buf *bp)
{
	struct psd_softc *sc;
	int part, nsectors, i, error;
	uint32_t part_offset;
	daddr_t blkno;
	uint8_t *data;

	sc = psd_lookup(bp->b_dev);
	if (sc == NULL || !sc->sc_attached) {
		bp->b_error = ENXIO;
		biodone(bp);
		return;
	}

	if (bp->b_bcount == 0) {
		biodone(bp);
		return;
	}

	/* Translate partition-relative blkno to absolute LBA */
	part = DISKPART(bp->b_dev);
	if (psd_part_offset(sc, part, &part_offset) != 0) {
		bp->b_error = ENXIO;
		biodone(bp);
		return;
	}

	blkno = bp->b_blkno + part_offset;
	data = bp->b_data;
	nsectors = bp->b_bcount / SD_SECTOR_SIZE;

	/* Polled sector-at-a-time read */
	error = 0;
	for (i = 0; i < nsectors; i++) {
		error = psd_sd_read_sector(sc, (uint32_t)(blkno + i),
		    data + i * SD_SECTOR_SIZE);
		if (error)
			break;
	}

	if (error) {
		bp->b_error = error;
		bp->b_resid = bp->b_bcount - i * SD_SECTOR_SIZE;
	} else {
		bp->b_resid = bp->b_bcount - nsectors * SD_SECTOR_SIZE;
	}
	biodone(bp);
}

static int
psdread(dev_t dev, struct uio *uio, int flags)
{

	return physio(psdstrategy, NULL, dev, B_READ, minphys, uio);
}

static int
psdwrite(dev_t dev, struct uio *uio, int flags)
{

	return physio(psdstrategy, NULL, dev, B_WRITE, minphys, uio);
}

static int
psdioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct psd_softc *sc;

	sc = psd_lookup(dev);
	if (sc == NULL)
		return ENXIO;

	switch (cmd) {
	case DIOCGDINFO: {
		struct disklabel *lp = data;
		int i;
		memset(lp, 0, sizeof(*lp));
		lp->d_secsize = SD_SECTOR_SIZE;
		lp->d_nsectors = 63;
		lp->d_ntracks = 255;
		lp->d_ncylinders = 1;
		lp->d_secpercyl = 63 * 255;
		lp->d_secperunit = 0xFFFFFFFF; /* unknown total */
		/* Raw partition covers entire device */
		lp->d_npartitions = PART_MBR_BASE + MBR_MAX_PARTS;
		lp->d_partitions[RAW_PART].p_offset = 0;
		lp->d_partitions[RAW_PART].p_size = lp->d_secperunit;
		lp->d_partitions[RAW_PART].p_fstype = FS_UNUSED;
		/* MBR partitions → e..h */
		for (i = 0; i < MBR_MAX_PARTS; i++) {
			int p = PART_MBR_BASE + i;
			lp->d_partitions[p].p_offset =
			    sc->sc_parts[i].offset;
			lp->d_partitions[p].p_size =
			    sc->sc_parts[i].size;
			lp->d_partitions[p].p_fstype =
			    sc->sc_parts[i].type != 0 ?
			    FS_OTHER : FS_UNUSED;
		}
		lp->d_magic = DISKMAGIC;
		lp->d_magic2 = DISKMAGIC;
		lp->d_checksum = dkcksum(lp);
		return 0;
	}
	default:
		return ENOTTY;
	}
}

static int
psdsize(dev_t dev)
{

	return -1;	/* unknown */
}

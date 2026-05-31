/*	$NetBSD$	*/

/*
 * pmci — Penumbra MMC Controller Interface (SD/MMC over SPI).
 *
 * Host controller driver that implements NetBSD's MI sdmmc_chip_functions
 * against the Penumbra SPI v2 register interface
 * (doc/system/devices/spi.md).  The hardware talks SPI; the SD protocol
 * is driven by the MI sdmmc(4) layer above us, which calls exec_command
 * with a fully-framed SD command and expects the response filled in.
 *
 * Attach path: pbbus -> pmci -> sdmmcbus -> sdmmc -> ld.
 *
 * Two transfer paths cooperate:
 *
 *   pmci_byte()   single-byte polled mode (FIFO_EN=0).  Used for the
 *                 6-byte command frame, the R1/R3/R7 response gap,
 *                 data-token polling, padding, CRC bytes, and the
 *                 post-write busy wait — all places where the byte
 *                 arrival time is unpredictable or the count is tiny.
 *
 *   pmci_burst()  N-byte FIFO-engine transfer.  Used for the bulk
 *                 c_datalen payload (typically the 512-byte block
 *                 data phase of CMD17/CMD24).  The hardware shift
 *                 engine pulls/pushes the FIFOs autonomously between
 *                 a single START and XFER_DONE, so the per-byte
 *                 CPU overhead that dominates the single-byte path
 *                 drops out entirely.
 *
 * FIFO bursting is gated on sc_fifo_depth >= len, with a single-byte
 * fallback if a future small-FIFO controller variant cannot fit the
 * payload.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kmem.h>

#include <dev/sdmmc/sdmmcchip.h>
#include <dev/sdmmc/sdmmcvar.h>
#include <dev/sdmmc/sdmmcreg.h>

#include <machine/bootinfo.h>
#include <machine/bus_defs.h>
#include <machine/bus_funcs.h>
#include <machine/pbbus.h>

/* ── SPI v2 register offsets (word-strided) ──────────────────── */

#define SPI_CAP		0x00
#define SPI_STATUS	0x04
#define SPI_CONTROL	0x08
#define SPI_DATA	0x0C
#define SPI_XFER_COUNT	0x10
#define SPI_IRQ_STATUS	0x14
#define SPI_IRQ_ENABLE	0x18

#define SPI_STATUS_BUSY		0x00000001

#define SPI_CTL_CS0		0x00000001
#define SPI_CTL_CS1		0x00000002
#define SPI_CTL_CPOL		0x00000010
#define SPI_CTL_CPHA		0x00000020
#define SPI_CTL_FAST		0x00000040
#define SPI_CTL_FIFO_EN		0x00000080
#define SPI_CTL_FLUSH_TX	0x00004000
#define SPI_CTL_FLUSH_RX	0x00008000

/* IRQ_STATUS bits.  Bit 0 is latched (W1C); bits 1-2 are live and
 * gated by fifo_en, so they read 0 when FIFO mode is not active. */
#define SPI_IRQ_XFER_DONE	0x00000001
#define SPI_IRQ_RX_THRESH	0x00000002
#define SPI_IRQ_TX_THRESH	0x00000004

/* Reset default: both CS deasserted, mode 0, slow clock, no FIFO. */
#define SPI_CTL_RESET	(SPI_CTL_CS0 | SPI_CTL_CS1)

/* ── SD-SPI protocol constants ────────────────────────────────── */

/*
 * Byte-level idle value.  The host sends 0xFF when it has nothing
 * meaningful to transmit (between command and response, between
 * bytes of a data transfer), and the card drives MISO high
 * (reads as 0xFF) when it has no response byte ready.
 */
#define SD_IDLE			0xFF

/*
 * Command frame: every command byte on MOSI starts with
 * bits [7:6] = 01 (start bit + host-to-card direction),
 * OR'd with the 6-bit command index.
 */
#define SD_CMD_TX_START		0x40
#define SD_CMD_INDEX_MASK	0x3F

/*
 * CRC7 + end bit.  Only CMD0 (GO_IDLE_STATE, arg=0) and CMD8
 * (SEND_IF_COND, arg=0x1AA) are required to carry a valid CRC in
 * SPI mode; for every other command the card ignores the CRC.
 * Hard-coded values are cheaper than a runtime CRC7 engine, since
 * the command arguments for these two are fixed.
 */
#define SD_CMD0_CRC		0x95	/* CRC7(0x40, 0) << 1 | 1 */
#define SD_CMD8_CRC		0x87	/* CRC7(0x48, 0x1AA) << 1 | 1 */

/*
 * Data-phase tokens.
 *   SD_DATA_TOKEN       : single-block read/write (CMD17, CMD24) and
 *                         every block of a multi-block READ (CMD18).
 *   SD_MBW_DATA_TOKEN   : multi-block WRITE (CMD25) — sent before each
 *                         block within the multi-block envelope.
 *   SD_MBW_STOP_TOKEN   : multi-block WRITE end-of-stream.  Replaces
 *                         the data token of the final block to signal
 *                         "no more blocks coming"; the card flushes and
 *                         drives busy until the last write commits.
 *                         No CRC and no data response follow it.
 */
#define SD_DATA_TOKEN		0xFE
#define SD_MBW_DATA_TOKEN	0xFC
#define SD_MBW_STOP_TOKEN	0xFD

/*
 * Write data response, returned one byte after the 16-bit CRC in
 * a block write.  Bits [4:0] hold the status code:
 *   0b00101 = accepted, 0b01011 = CRC error, 0b01101 = write error.
 * Bits [7:5] are undefined and must be masked off before comparing.
 */
#define SD_DATA_RESP_MASK	0x1F
#define SD_DATA_RESP_ACCEPTED	0x05

/*
 * Retry bounds for polled waits.  Chosen for *real-hardware*
 * worst-case timing, not ISS speed — the ISS completes each byte
 * in a few simulation cycles, so these bounds are effectively
 * instant there, while on the FPGA they give the wall-clock
 * envelope the SD spec requires.
 *
 * Approximate envelopes at FAST SPI clock (~6.25 MHz bit rate,
 * ~1.3 µs per byte including handshake):
 *
 *   SD_RESP_RETRIES          — Ncr: 0..8 bytes (SD spec hard limit)
 *   SD_DATA_TOKEN_RETRIES    — Nac: ~100 ms typical, up to 1 s
 *   SD_BUSY_RETRIES          — Nbr: up to 250 ms SD 2.0,
 *                              ~500 ms SDHC worst case
 *
 * On the SLOW clock (~400 kHz, ~20 µs per byte) the same retry
 * counts yield ~15× longer wall-clock envelopes — fine as safety
 * bounds, since SLOW is only used during card initialisation.
 */
#define SD_RESP_RETRIES		8		/*    ~10 µs at FAST */
#define SD_DATA_TOKEN_RETRIES	100000		/*  ~130 ms at FAST */
#define SD_BUSY_RETRIES		500000		/*  ~650 ms at FAST */

/* ── Softc ────────────────────────────────────────────────────── */

struct pmci_softc {
	device_t		sc_dev;
	device_t		sc_sdmmc;		/* attached bus */
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	uint32_t		sc_fifo_depth;		/* from CAP */
	uint32_t		sc_version;		/* CAP version */
};

static int	pmci_match(device_t, cfdata_t, void *);
static void	pmci_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(pmci, sizeof(struct pmci_softc),
    pmci_match, pmci_attach, NULL, NULL);

/* ── Forward declarations: sdmmc_chip_functions ───────────────── */

static int	pmci_host_reset(sdmmc_chipset_handle_t);
static uint32_t	pmci_host_ocr(sdmmc_chipset_handle_t);
static int	pmci_host_maxblklen(sdmmc_chipset_handle_t);
static int	pmci_card_detect(sdmmc_chipset_handle_t);
static int	pmci_write_protect(sdmmc_chipset_handle_t);
static int	pmci_bus_power(sdmmc_chipset_handle_t, uint32_t);
static int	pmci_bus_clock(sdmmc_chipset_handle_t, int);
static int	pmci_bus_width(sdmmc_chipset_handle_t, int);
static void	pmci_exec_command(sdmmc_chipset_handle_t,
		    struct sdmmc_command *);

static void	pmci_spi_initialize(sdmmc_chipset_handle_t);

static struct sdmmc_chip_functions pmci_chip_functions = {
	.host_reset		= pmci_host_reset,
	.host_ocr		= pmci_host_ocr,
	.host_maxblklen		= pmci_host_maxblklen,
	.card_detect		= pmci_card_detect,
	.write_protect		= pmci_write_protect,
	.bus_power		= pmci_bus_power,
	.bus_clock		= pmci_bus_clock,
	.bus_width		= pmci_bus_width,
	.exec_command		= pmci_exec_command,
	.card_enable_intr	= NULL,		/* no SDIO */
	.card_intr_ack		= NULL,
};

static struct sdmmc_spi_chip_functions pmci_spi_chip_functions = {
	.initialize		= pmci_spi_initialize,
};

/* ── Low-level register helpers ───────────────────────────────── */

#define	SREG_RD(sc, r)		\
	bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (r))
#define	SREG_WR(sc, r, v)	\
	bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (r), (v))
#define	SREG_SET(sc, r, b)	SREG_WR((sc), (r), SREG_RD((sc), (r)) | (b))
#define	SREG_CLR(sc, r, b)	SREG_WR((sc), (r), SREG_RD((sc), (r)) & ~(b))

/*
 * Shift a single byte through the SPI engine (polled, FIFO_EN=0).
 * Returns the byte received on MISO during the shift.
 */
static inline uint8_t
pmci_byte(struct pmci_softc *sc, uint8_t tx)
{

	SREG_WR(sc, SPI_DATA, tx);
	while (SREG_RD(sc, SPI_STATUS) & SPI_STATUS_BUSY)
		;
	return (uint8_t)SREG_RD(sc, SPI_DATA);
}

static inline void
pmci_cs(struct pmci_softc *sc, int assert)
{

	/* CS0 is active low on the pin; bit 0 high = deasserted. */
	if (assert)
		SREG_CLR(sc, SPI_CONTROL, SPI_CTL_CS0);
	else
		SREG_SET(sc, SPI_CONTROL, SPI_CTL_CS0);
}

/* ── sdmmc_chip_functions implementation ──────────────────────── */

static int
pmci_host_reset(sdmmc_chipset_handle_t sch)
{
	struct pmci_softc *sc = sch;

	/* Back to the boot-ROM / psd-compatible baseline:
	 * both CS deasserted, slow clock, FIFO disabled. */
	SREG_WR(sc, SPI_CONTROL, SPI_CTL_RESET);
	SREG_WR(sc, SPI_IRQ_ENABLE, 0);
	return 0;
}

static uint32_t
pmci_host_ocr(sdmmc_chipset_handle_t sch)
{

	/* SD cards run at 3.3V; we have no voltage-switching hardware. */
	return MMC_OCR_3_2V_3_3V | MMC_OCR_3_3V_3_4V;
}

static int
pmci_host_maxblklen(sdmmc_chipset_handle_t sch)
{

	return 512;
}

static int
pmci_card_detect(sdmmc_chipset_handle_t sch)
{

	/*
	 * Card-detect is not available on the current hardware.
	 * On the ULX3S the microSD CD trace is marked "not connected"
	 * in the board constraints (sd_cdn at N5), and spi.sv does
	 * not expose a CD register anyway — so there is no signal
	 * to read either at the board or the RTL level.  We report
	 * "present" unconditionally; missing cards surface as a
	 * CMD0 timeout during MI-layer discovery, which is handled
	 * as a degraded attach.  If a future board/RTL pair does
	 * route CD, this becomes a live read via a new register.
	 */
	return 1;
}

static int
pmci_write_protect(sdmmc_chipset_handle_t sch)
{

	/*
	 * microSD cards have no write-protect mechanism — the physical
	 * slide-switch is a full-size-SD-only feature.  Always report
	 * writable; there is no hardware signal that could tell us
	 * otherwise.
	 */
	return 0;
}

static int
pmci_bus_power(sdmmc_chipset_handle_t sch, uint32_t ocr)
{

	if ((ocr & (MMC_OCR_3_2V_3_3V | MMC_OCR_3_3V_3_4V)) == 0)
		return EINVAL;
	return 0;	/* power is always on; nothing to gate */
}

static int
pmci_bus_clock(sdmmc_chipset_handle_t sch, int freq)
{
	struct pmci_softc *sc = sch;

	/*
	 * Two hardware-parameterized speeds; software picks one:
	 *   freq == SDMMC_SDCLK_OFF  (0)   — idle; leave clock slow
	 *   freq == SDMMC_SDCLK_400K (400) — init phase, use SLOW
	 *   freq >= anything else          — data phase, use FAST
	 */
	if (freq <= SDMMC_SDCLK_400K)
		SREG_CLR(sc, SPI_CONTROL, SPI_CTL_FAST);
	else
		SREG_SET(sc, SPI_CONTROL, SPI_CTL_FAST);
	return 0;
}

static int
pmci_bus_width(sdmmc_chipset_handle_t sch, int width)
{

	/* SPI mode is 1-bit by definition. */
	return (width == 1) ? 0 : EINVAL;
}

/*
 * Pre-card initialization: send >= 74 clocks with CS deasserted and
 * MOSI high, then assert CS0.  Required by the SD spec (section 7.2.1)
 * to pull the card out of its power-on SPI-negotiation state.  Called
 * once by sdmmc_mem_enable() before any CMD0.
 */
static void
pmci_spi_initialize(sdmmc_chipset_handle_t sch)
{
	struct pmci_softc *sc = sch;
	int i;

	/* Slow clock, FIFO off, both CS high. */
	SREG_WR(sc, SPI_CONTROL, SPI_CTL_RESET);

	/* 80 clocks (10 bytes) with CS deasserted is the psd-proven
	 * pattern; scimci does 20 for extra margin.  Match scimci. */
	for (i = 0; i < 20; i++)
		(void)pmci_byte(sc, 0xFF);

	/* Assert CS0 for subsequent CMD0. */
	pmci_cs(sc, 1);
}

static uint8_t
pmci_cmd_crc(uint8_t cmd)
{
	switch (cmd) {
	case MMC_GO_IDLE_STATE:
		return SD_CMD0_CRC;
	case SD_SEND_IF_COND:
		return SD_CMD8_CRC;
	default:
		return SD_IDLE;		/* ignored by the card */
	}
}

/*
 * pmci_wait_token — spin on MISO until the card stops sending idle
 * bytes (0xFF), or the retry bound trips.  Returns 0 and stores the
 * received byte on success, ETIMEDOUT on exhaustion.
 *
 * Used for both R1 response polling (R1 has bit 7 clear, so any
 * non-0xFF byte is a valid R1) and data-token polling (where the
 * caller verifies tok == 0xFE after success).
 */
static int
pmci_wait_token(struct pmci_softc *sc, int retries, uint8_t *tok)
{
	uint8_t b;
	int i;

	for (i = 0; i < retries; i++) {
		b = pmci_byte(sc, SD_IDLE);
		if (b != SD_IDLE) {
			*tok = b;
			return 0;
		}
	}
	return ETIMEDOUT;
}

/*
 * pmci_wait_busy — spin until the card releases MISO back to idle
 * (0xFF).  Cards hold MISO low after writes and R1b commands while
 * the internal operation completes.  SD spec Nbr can be ~250 ms
 * worst case; SD_BUSY_RETRIES at ~1 µs/byte gives an ~85 ms envelope,
 * which is adequate for the ISS and typical SDHC cards.
 *
 * Returns success on the first 0xFF byte: appropriate for R1b
 * commands (CMD12, CMD13, CMD7) where the busy period may be
 * shorter than one byte time on fast operations.  See
 * pmci_wait_busy_strict() for the multi-block-write variant that
 * additionally requires having seen busy before exiting.
 */
static int
pmci_wait_busy(struct pmci_softc *sc, int retries)
{
	int i;

	for (i = 0; i < retries; i++) {
		if (pmci_byte(sc, SD_IDLE) == SD_IDLE)
			return 0;
	}
	return ETIMEDOUT;
}

/*
 * pmci_wait_busy_strict — wait for busy assertion AND release.
 *
 * Between the data-response token and the card's actual busy
 * assertion, there can be a 1-byte 0xFF gap.  A "wait until first
 * 0xFF" loop exits at that gap byte, declaring "busy released"
 * before the card was even busy.  For single-block writes and
 * generic R1b commands this is harmless — nothing else happens on
 * the SPI bus afterwards.  For multi-block CMD25 it's a real bug:
 * the next 0xFC token lands while the card is still programming,
 * and the card silently misses the block.  Empirically observed as
 * intermittent "error writing fsbn ..." with successful retry.
 *
 * Use only inside the multi-block-write block-step loop and the
 * trailing 0xFD-stop-tran flush, where the card is guaranteed to
 * assert busy.
 */
static int
pmci_wait_busy_strict(struct pmci_softc *sc, int retries)
{
	bool saw_busy = false;
	int i;

	for (i = 0; i < retries; i++) {
		uint8_t b = pmci_byte(sc, SD_IDLE);
		if (b != SD_IDLE)
			saw_busy = true;
		else if (saw_busy)
			return 0;
	}
	return ETIMEDOUT;
}

/*
 * pmci_burst — shift `len` bytes through the SPI v2 FIFO engine.
 *
 * Used for the 512-byte data phase of read/write block commands,
 * where per-byte polled overhead in pmci_byte() dominates wall-clock
 * cost.  The engine pulls TX bytes / pushes RX bytes autonomously
 * between a single START write and the XFER_DONE signal — no CPU
 * involvement in the inner shift, which is the entire point.
 *
 *   tx_buf == NULL : drive idle (0xFF) on MOSI; RX bytes land in rx_buf.
 *   rx_buf == NULL : drive tx_buf on MOSI; received bytes are discarded.
 *
 * Preconditions on entry:
 *   - shift register idle (caller's last pmci_byte() returned, which
 *     already spun on SPI_STATUS.BUSY)
 *   - CONTROL holds the SD-protocol state (CS0 asserted, FAST/SLOW
 *     clock as picked by pmci_bus_clock()).  We preserve and restore
 *     CONTROL across the burst, so the caller's CS/clock state is
 *     unchanged on return.
 *   - `len` <= sc_fifo_depth (caller has already gated on this; the
 *     transfer engine itself would happily handle longer bursts with
 *     mid-flight refills, but the polled-completion path here assumes
 *     "fill TX → START → wait → drain RX" fits in one shot)
 *
 * The XFER_DONE poll uses the same retry budget as pmci_wait_busy
 * (~650 ms envelope at 1 cycle / iteration of SPR-style RD latency).
 * Wall-clock for a 512-byte burst is ~660 µs at FAST and ~10.5 ms at
 * SLOW, so the budget covers both with ample margin.
 *
 * Returns 0 on success, ETIMEDOUT if XFER_DONE never arrives.  Even
 * on timeout CONTROL is restored, so the caller's subsequent
 * pmci_byte() reach the wire on a clean baseline.
 */
static int
pmci_burst(struct pmci_softc *sc, const uint8_t *tx_buf, uint8_t *rx_buf,
    int len)
{
	uint32_t ctl;
	int i, retries;

	KASSERT((SREG_RD(sc, SPI_STATUS) & SPI_STATUS_BUSY) == 0);

	/*
	 * Snapshot CONTROL, then flip into FIFO mode while flushing
	 * both FIFOs in the same write.  CS/FAST/mode bits ride along
	 * unchanged; FLUSH_TX/FLUSH_RX are write-1-pulse and don't
	 * persist.  IRQ_ENABLE stays at zero across the burst — at
	 * current scheduler costs (~10 ms per cv_wait round-trip vs
	 * a 660 µs burst) sleeping on XFER_DONE costs ~2× more than it
	 * saves; polled busy-wait remains correct until either context
	 * switches get cheaper or CMD18 multi-block makes per-burst
	 * waits long enough to amortize the round-trip.
	 */
	ctl = SREG_RD(sc, SPI_CONTROL);
	SREG_WR(sc, SPI_CONTROL,
	    ctl | SPI_CTL_FIFO_EN | SPI_CTL_FLUSH_TX | SPI_CTL_FLUSH_RX);
	SREG_WR(sc, SPI_IRQ_ENABLE, 0);

	/*
	 * Fill the 512-deep TX FIFO via the bus_space "multi" primitives:
	 * write_multi_1 streams a buffer into the single SPI_DATA
	 * register; set_multi_1 fills it with a constant idle byte.  Both
	 * compile to a tight loop with the handle pinned in a register —
	 * unlike open-coded SREG_WR, which the kernel's
	 * -fno-strict-aliasing forces to reload sc->sc_ioh before every
	 * store (the load can't be proven not to alias the MMIO write).
	 */
	if (tx_buf)
		bus_space_write_multi_1(sc->sc_iot, sc->sc_ioh, SPI_DATA,
		    tx_buf, len);
	else
		bus_space_set_multi_1(sc->sc_iot, sc->sc_ioh, SPI_DATA,
		    SD_IDLE, len);

	SREG_WR(sc, SPI_XFER_COUNT, ((uint32_t)len & 0xFFFF) | (1U << 16));

	for (retries = SD_BUSY_RETRIES; retries > 0; retries--) {
		if (SREG_RD(sc, SPI_IRQ_STATUS) & SPI_IRQ_XFER_DONE)
			break;
	}
	if (retries == 0) {
		SREG_WR(sc, SPI_CONTROL, ctl);
		return ETIMEDOUT;
	}
	SREG_WR(sc, SPI_IRQ_STATUS, SPI_IRQ_XFER_DONE);

	/*
	 * Drain the RX FIFO.  read_multi_1 pops the buffered case; the
	 * discard case has no standard primitive, so it stays open-coded
	 * (a bare volatile read forces no sc->sc_ioh reload anyway).
	 */
	if (rx_buf) {
		bus_space_read_multi_1(sc->sc_iot, sc->sc_ioh, SPI_DATA,
		    rx_buf, len);
	} else {
		for (i = 0; i + 8 <= len; i += 8) {
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
			(void)SREG_RD(sc, SPI_DATA);
		}
		for (; i < len; i++)
			(void)SREG_RD(sc, SPI_DATA);
	}

	SREG_WR(sc, SPI_CONTROL, ctl);
	return 0;
}

/*
 * pmci_read — handle the data phase of a block read.
 *
 * Covers both CMD17 (READ_BLOCK_SINGLE, c_opcode == 17, c_datalen == 512)
 * and CMD18 (READ_BLOCK_MULTIPLE, c_opcode == 18, c_datalen == N × 512).
 * Per the SD-SPI spec the per-block framing is identical: data token
 * 0xFE, 512 data bytes, 2 CRC bytes.  In CMD18 the card streams blocks
 * back-to-back with the next 0xFE token appearing 0..Nac idle bytes
 * after the previous CRC; the token poll loop handles that gap
 * naturally.  CMD12 (STOP_TRANSMISSION) is issued by the MI sdmmc
 * layer as a separate exec_command call after we return.
 */
static void
pmci_read(struct pmci_softc *sc, struct sdmmc_command *cmd)
{
	uint8_t *data = cmd->c_data;
	uint8_t tok;
	int error, i, b, nblocks;
	const int blklen = cmd->c_blklen;

	/*
	 * Contract from sdmmc_mem.c: c_blklen is 512 for the normal
	 * read/write path (CMD17/18/24/25) and equal to c_datalen for
	 * SPI-mode CSD/CID/EXT_CSD reads (sdmmc_mem_send_cxd_data,
	 * blklen = 16 for CSD/CID, 512 for EXT_CSD).  Either way the
	 * per-block framing on the wire is identical — token + blklen
	 * bytes + 2 CRC — and c_datalen is always a whole multiple of
	 * c_blklen.  Assert that invariant directly.
	 */
	KASSERT(blklen > 0);
	KASSERT(cmd->c_datalen > 0 && (cmd->c_datalen % blklen) == 0);

	nblocks = cmd->c_datalen / blklen;

	for (b = 0; b < nblocks; b++) {
		/*
		 * Token poll stays single-byte: SD spec Nac allows up to
		 * ~100 ms between R1 (or the previous block's CRC, for
		 * CMD18) and the next data-start token.  Bursting would
		 * over-read past the token boundary.
		 */
		error = pmci_wait_token(sc, SD_DATA_TOKEN_RETRIES, &tok);
		if (error) {
			cmd->c_error = error;
			return;
		}
		if (tok != SD_DATA_TOKEN) {
			cmd->c_error = EIO;
			return;
		}

		if (sc->sc_fifo_depth >= (uint32_t)blklen) {
			error = pmci_burst(sc, NULL, data, blklen);
			if (error) {
				cmd->c_error = error;
				return;
			}
		} else {
			for (i = 0; i < blklen; i++)
				data[i] = pmci_byte(sc, SD_IDLE);
		}

		/* Discard 16-bit CRC. */
		(void)pmci_byte(sc, SD_IDLE);
		(void)pmci_byte(sc, SD_IDLE);

		data += blklen;
	}
}

/*
 * pmci_write — handle the data phase of a block write.
 *
 * CMD24 (single, c_opcode == 24) and CMD25 (multi, c_opcode == 25)
 * share most of the framing — pad + data token + 512 bytes + CRC +
 * data-response token + busy-wait per block — but differ in the
 * token used for each block and in how the transfer is closed:
 *
 *   CMD24: data token = 0xFE; after the single block's busy-wait,
 *          we return and the MI sdmmc layer drives no follow-up.
 *
 *   CMD25: data token = 0xFC for every block; after the last block's
 *          busy-wait, send a pad byte and a 0xFD "stop tran" token
 *          (no CRC, no response token) and wait for the card's final
 *          busy to release.  The MI sdmmc layer separately issues
 *          CMD12 STOP_TRANSMISSION after we return.
 */
static void
pmci_write(struct pmci_softc *sc, struct sdmmc_command *cmd)
{
	const uint8_t *data = cmd->c_data;
	uint8_t resp;
	int error, i, b, nblocks;
	const int blklen = cmd->c_blklen;
	const bool multi = (cmd->c_opcode == MMC_WRITE_BLOCK_MULTIPLE);
	const uint8_t token = multi ? SD_MBW_DATA_TOKEN : SD_DATA_TOKEN;

	KASSERT(blklen > 0);
	KASSERT(cmd->c_datalen > 0 && (cmd->c_datalen % blklen) == 0);

	nblocks = cmd->c_datalen / blklen;

	for (b = 0; b < nblocks; b++) {
		/*
		 * One pad byte before each data token.  For the first
		 * block this is Nwr (the SD-spec gap between R1 and the
		 * first 0xFE/0xFC).  For subsequent blocks in a CMD25
		 * envelope it gives the card a clock-beat between busy
		 * release and the next 0xFC — empirically required by
		 * cards in the wild (intermittent "error writing fsbn"
		 * + successful retry without it), and matches what
		 * Linux/U-Boot SPI-mode drivers do.  Harmless on the
		 * single-block path; the card just clocks an extra
		 * idle byte.
		 */
		(void)pmci_byte(sc, SD_IDLE);
		(void)pmci_byte(sc, token);

		if (sc->sc_fifo_depth >= (uint32_t)blklen) {
			error = pmci_burst(sc, data, NULL, blklen);
			if (error) {
				cmd->c_error = error;
				return;
			}
		} else {
			for (i = 0; i < blklen; i++)
				(void)pmci_byte(sc, data[i]);
		}

		/* Dummy CRC16 — ignored by the card in SPI mode. */
		(void)pmci_byte(sc, SD_IDLE);
		(void)pmci_byte(sc, SD_IDLE);

		/*
		 * Data-response token: SD spec allows 0..8 byte gap
		 * (Ncrc) before it appears.  Packed 0bxxx0rrr1; rrr=010
		 * (0x05 in the low 5 bits) means accepted.
		 */
		error = pmci_wait_token(sc, SD_RESP_RETRIES, &resp);
		if (error) {
			cmd->c_error = error;
			return;
		}
		if ((resp & SD_DATA_RESP_MASK) != SD_DATA_RESP_ACCEPTED) {
			cmd->c_error = EIO;
			return;
		}

		/*
		 * Card drives MISO low while programming; wait for release
		 * before the next 0xFC token (or the 0xFD stop token).
		 * Use the strict variant: if the next 0xFC went out
		 * before the card finished, the card silently misses the
		 * block — observed as intermittent "error writing fsbn".
		 */
		if (pmci_wait_busy_strict(sc, SD_BUSY_RETRIES) != 0) {
			cmd->c_error = ETIMEDOUT;
			return;
		}

		data += blklen;
	}

	if (multi) {
		/*
		 * End-of-stream for CMD25: pad byte + 0xFD stop token.
		 * No CRC, no data response — the card just enters busy
		 * until the final block has flushed to flash.  Strict
		 * busy wait because the card *will* assert busy here,
		 * and the next exec_command (CMD12 from the MI layer)
		 * needs the card fully ready.
		 */
		(void)pmci_byte(sc, SD_IDLE);
		(void)pmci_byte(sc, SD_MBW_STOP_TOKEN);
		if (pmci_wait_busy_strict(sc, SD_BUSY_RETRIES) != 0)
			cmd->c_error = ETIMEDOUT;
	}
}

/*
 * pmci_exec_command — translate a framed SD command into an SPI byte
 * sequence, collect the response, and handle any data-phase transfer.
 *
 * Inputs (struct sdmmc_command):
 *   c_opcode  — SD command index (0..63)
 *   c_arg     — 32-bit command argument
 *   c_flags   — response type (SCF_RSP_SPI_R1 .. R7, bits 10..13)
 *               and data direction (SCF_CMD_READ set for data-in)
 *   c_data    — pointer to buffer for data-phase transfer
 *   c_datalen — bytes to transfer (0 if no data phase)
 *
 * Outputs:
 *   c_resp[0] — packed response:
 *                 R1         : low byte = R1 token
 *                 R2 (S2)    : low 16 bits = (R1 << 8) | trailing-byte
 *                 R3/R7 (B4) : byte 0 = R1 token, byte 1..4 = big-endian
 *                              trailing four-byte payload (OCR, IF cond)
 *   c_error   — 0 on success, else ETIMEDOUT / EIO
 *   c_flags  |= SCF_ITSDONE
 *
 * Reference implementation: netbsd/sys/arch/evbsh3/t_sh7706lan/scimci.c
 * (scimci_exec_command) is the canonical NetBSD SPI-mode host driver
 * and the closest structural analogue to this code.
 *
 * Data-phase notes:
 *   - Read  (SCF_CMD_READ set): after R1, poll for 0xFE data token, read
 *     c_datalen bytes, then consume two CRC bytes (discarded).
 *   - Write (SCF_CMD_READ clear): after R1, send one 0xFF pad, send 0xFE
 *     start token, send c_datalen bytes, send 2 dummy CRC bytes, poll for
 *     "data response" byte (should be 0bxxx00101 for accept), then wait
 *     for card to leave busy (MISO goes high).
 *
 * The MI sdmmc layer retries on ETIMEDOUT for probe commands marked
 * SCF_TOUT_OK — don't panic on timeout during initial card detection.
 */
static void
pmci_exec_command(sdmmc_chipset_handle_t sch, struct sdmmc_command *cmd)
{
	struct pmci_softc *sc = sch;
	uint8_t r1;
	int error;

	cmd->c_error = 0;
	cmd->c_resp[0] = 0;
	cmd->c_resp[1] = 0;
	cmd->c_resp[2] = 0;
	cmd->c_resp[3] = 0;

	/* 6-byte command frame: start-bit+index, 4-byte arg, CRC7+stop-bit. */
	(void)pmci_byte(sc, SD_CMD_TX_START | (cmd->c_opcode & SD_CMD_INDEX_MASK));
	(void)pmci_byte(sc, (cmd->c_arg >> 24) & 0xFF);
	(void)pmci_byte(sc, (cmd->c_arg >> 16) & 0xFF);
	(void)pmci_byte(sc, (cmd->c_arg >>  8) & 0xFF);
	(void)pmci_byte(sc, (cmd->c_arg      ) & 0xFF);
	(void)pmci_byte(sc, pmci_cmd_crc(cmd->c_opcode));

	/* R1 token: 0..8 byte gap, identified by bit 7 == 0. */
	error = pmci_wait_token(sc, SD_RESP_RETRIES, &r1);
	if (error) {
		cmd->c_error = error;
		goto out;
	}

	/*
	 * Every SPI-mode response starts with the R1 byte; longer
	 * responses append extra bytes.  Always store R1 first, then
	 * consume S2 or B4 trailing bytes if the MI layer asked for
	 * them.  Checking S1 as a branch would be wrong — S1 is set
	 * for every response type (R1/R1b/R2/R3/R4/R5/R7), so an
	 * "if S1 else if S2 else if B4" chain never reaches S2/B4.
	 */
	cmd->c_resp[0] = r1;
	if (cmd->c_flags & SCF_RSP_SPI_S2) {
		cmd->c_resp[0] |= (uint32_t)pmci_byte(sc, SD_IDLE) << 8;
	} else if (cmd->c_flags & SCF_RSP_SPI_B4) {
		cmd->c_resp[1]  = (uint32_t)pmci_byte(sc, SD_IDLE) << 24;
		cmd->c_resp[1] |= (uint32_t)pmci_byte(sc, SD_IDLE) << 16;
		cmd->c_resp[1] |= (uint32_t)pmci_byte(sc, SD_IDLE) <<  8;
		cmd->c_resp[1] |= (uint32_t)pmci_byte(sc, SD_IDLE);
	}

	/* R1b: card drives MISO low while the internal op runs. */
	if ((cmd->c_flags & SCF_RSP_SPI_BSY) &&
	    pmci_wait_busy(sc, SD_BUSY_RETRIES) != 0) {
		cmd->c_error = ETIMEDOUT;
		goto out;
	}

	if (cmd->c_datalen > 0) {
		if (cmd->c_flags & SCF_CMD_READ) {
			pmci_read(sc, cmd);
			/*
			 * CMD9 (SEND_CSD) / CMD10 (SEND_CID) deliver
			 * 16 MSB-first wire bytes, where byte 15 is the
			 * CRC7+end-bit and bytes 0..14 are the upper
			 * 120 bits of CSD/CID content.  The MI decoder
			 * reads c_resp via MMC_RSP_BITS, which expects
			 * register bit N at resp bit (N-8) — i.e. the
			 * content is shifted down by 8 so the low 8
			 * bits of resp are the CRC slot.  Reverse
			 * bytes 0..14 into c_data bytes 0..14 and zero
			 * byte 15 to produce that layout directly.
			 * EXT_CSD is 512 bytes, byte-indexed, and needs
			 * no reordering.
			 */
			if (cmd->c_error == 0 &&
			    (cmd->c_opcode == MMC_SEND_CID ||
			     cmd->c_opcode == MMC_SEND_CSD) &&
			    cmd->c_datalen == 16) {
				uint8_t *w = cmd->c_data;
				uint8_t tmp[16];
				int i;
				for (i = 0; i < 15; i++)
					tmp[i] = w[14 - i];
				tmp[15] = 0;
				memcpy(w, tmp, 16);
			}
		} else {
			pmci_write(sc, cmd);
		}
	}

out:
	cmd->c_flags |= SCF_ITSDONE;
}

/* ── Autoconf ─────────────────────────────────────────────────── */

static int
pmci_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pbbus_attach_args *pa = aux;

	return pa->pb_class == ACFG_CLASS_SD;
}

static void
pmci_attach(device_t parent, device_t self, void *aux)
{
	struct pmci_softc *sc = device_private(self);
	struct pbbus_attach_args *pa = aux;
	struct sdmmcbus_attach_args saa;
	uint32_t cap;
	int error;

	sc->sc_dev = self;
	sc->sc_iot = pa->pb_iot;

	error = bus_space_map(sc->sc_iot, pa->pb_addr, pa->pb_size,
	    0, &sc->sc_ioh);
	if (error) {
		aprint_error(": can't map SPI registers: %d\n", error);
		return;
	}

	/* Reset the controller into the known-good baseline. */
	(void)pmci_host_reset(sc);

	/* CAP: version (bits 7:0) and FIFO depth (bits 23:8). */
	cap = SREG_RD(sc, SPI_CAP);
	sc->sc_version = cap & 0xFF;
	sc->sc_fifo_depth = (cap >> 8) & 0xFFFF;

	aprint_naive("\n");
	aprint_normal(": Penumbra SPI SD/MMC host (v%u, FIFO %u)\n",
	    sc->sc_version, sc->sc_fifo_depth);

	/*
	 * Hand off to the MI sdmmc(4) bus.  The MI layer owns card
	 * discovery, CSD/CID parsing, and block-device attachment —
	 * we just shovel bytes on its behalf.
	 */
	memset(&saa, 0, sizeof(saa));
	saa.saa_busname = "sdmmc";
	saa.saa_sct = &pmci_chip_functions;
	saa.saa_spi_sct = &pmci_spi_chip_functions;
	saa.saa_sch = sc;
	/*
	 * Clock range advertised to the MI sdmmc layer.  These are
	 * SD-card-class nominal limits, not a description of what
	 * our hardware actually delivers: 400 kHz is the SD init
	 * requirement and 25 MHz is the Default-Speed class upper
	 * bound.  The real clock is chosen by the hardware from its
	 * FAST/SLOW parameters; pmci_bus_clock() just picks which
	 * of the two to engage based on the MI layer's request.
	 * This keeps the driver independent of system clock and of
	 * whether the SPI controller shares a clock domain with
	 * the CPU — both of which can change between FPGA and
	 * future discrete builds.
	 */
	saa.saa_clkmin = 400;
	saa.saa_clkmax = 25000;
	/*
	 * SMC_CAPS_SINGLE_ONLY is *not* set: pmci_read and pmci_write
	 * handle CMD18 (READ_BLOCK_MULTIPLE) and CMD25 (WRITE_BLOCK_MULTIPLE)
	 * natively, so the MI sdmmc layer can amortize its ~3.7 ms/sector
	 * overhead across a whole syscall.  CMD12 STOP_TRANSMISSION is
	 * issued by the MI layer as a separate exec_command after the
	 * data phase (we don't set SMC_CAPS_AUTO_STOP either).
	 */
	saa.saa_caps = SMC_CAPS_SPI_MODE
		     | SMC_CAPS_POLL_CARD_DET;

	sc->sc_sdmmc = config_found(self, &saa, NULL, CFARGS_NONE);
	if (sc->sc_sdmmc == NULL)
		aprint_error_dev(self, "couldn't attach sdmmc bus\n");
}

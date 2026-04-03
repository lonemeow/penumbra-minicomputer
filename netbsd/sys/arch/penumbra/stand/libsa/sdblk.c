/*
 * sdblk.c — SD card block device for libsa standalone bootloader
 *
 * Bridges Penumbra's SPI-mode SD card to libsa's block device
 * interface (strategy/open/close/ioctl).  Self-contained: duplicates
 * the SPI and SD-SPI protocol from hw/rom/ rather than depending
 * on ROM code.
 *
 * The SPI controller base address and boot device info come from
 * the Penumbra boot data tagged list, passed via R1 from the ROM.
 */

#include <lib/libsa/stand.h>

/*
 * We need the boot data structures.  These are defined in hw/rom/bootdata.h
 * but that header depends on hw/rom/penumbra.h (which provides uint32_t).
 * Since we're compiling in a NetBSD environment where uint32_t comes from
 * <sys/types.h>, we define the boot data structures locally.
 */

/* Boot data base address and tag types — must match hw/rom/bootdata.h */
#define BOOTDATA_MAGIC   0x50454E42  /* "PENB" */

#define BTAG_END       0
#define BTAG_MEMORY    1
#define BTAG_DEVICE    2
#define BTAG_CONSOLE   3
#define BTAG_BOOTDEV   4

struct btag_hdr {
	uint32_t type;
	uint32_t size;
};

struct bootdata_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t total_size;
};

struct btag_device {
	struct btag_hdr hdr;
	uint32_t cls;
	uint32_t base;
	uint32_t dev_size;
	uint32_t id;
	char     name[16];
};

struct btag_bootdev {
	struct btag_hdr hdr;
	uint32_t dev_nth;
	uint32_t cs;
	uint32_t partition;
};

/* ── SPI register interface ──────────────────────────────────────── */

#define SPI_DATA     0
#define SPI_STATUS   1
#define SPI_CONTROL  2
#define SPI_CLKDIV   3
#define SPI_STATUS_BUSY  0x01

static unsigned char
spi_transfer(uint32_t base, unsigned char tx)
{
	volatile uint32_t *regs = (volatile uint32_t *)base;

	regs[SPI_DATA] = tx;
	while (regs[SPI_STATUS] & SPI_STATUS_BUSY)
		;
	return (unsigned char)regs[SPI_DATA];
}

static void
spi_cs0(uint32_t base, int state)
{
	volatile uint32_t *regs = (volatile uint32_t *)base;
	uint32_t ctl = regs[SPI_CONTROL];

	if (state)
		ctl |= 0x01u;
	else
		ctl &= ~0x01u;
	regs[SPI_CONTROL] = ctl;
}

static void
spi_set_clkdiv(uint32_t base, unsigned int div)
{
	volatile uint32_t *regs = (volatile uint32_t *)base;

	regs[SPI_CLKDIV] = div;
}

/* ── SD SPI protocol ─────────────────────────────────────────────── */

#define SD_R1_IDLE       0x01
#define SD_R1_NO_RESP    0xFF
#define SD_CMD0_RETRIES   20
#define SD_ACMD41_RETRIES 1500
#define SD_DATA_RETRIES   1000
#define SD_RESP_RETRIES   8

static unsigned char
sd_command(uint32_t base, unsigned char cmd, uint32_t arg)
{
	unsigned char r;

	spi_transfer(base, 0x40 | cmd);
	spi_transfer(base, (unsigned char)(arg >> 24));
	spi_transfer(base, (unsigned char)(arg >> 16));
	spi_transfer(base, (unsigned char)(arg >> 8));
	spi_transfer(base, (unsigned char)(arg));
	if (cmd == 0)
		spi_transfer(base, 0x95);
	else if (cmd == 8)
		spi_transfer(base, 0x87);
	else
		spi_transfer(base, 0xFF);

	for (int i = 0; i < SD_RESP_RETRIES; i++) {
		r = spi_transfer(base, 0xFF);
		if (!(r & 0x80))
			return r;
	}
	return r;
}

static uint32_t
sd_read_response32(uint32_t base)
{
	uint32_t val;

	val  = (uint32_t)spi_transfer(base, 0xFF) << 24;
	val |= (uint32_t)spi_transfer(base, 0xFF) << 16;
	val |= (uint32_t)spi_transfer(base, 0xFF) << 8;
	val |= (uint32_t)spi_transfer(base, 0xFF);
	return val;
}

static int
sd_init(uint32_t base)
{
	unsigned char r1;

	spi_set_clkdiv(base, 0xFF);
	spi_cs0(base, 1);
	for (int i = 0; i < 20; i++)
		spi_transfer(base, 0xFF);

	spi_cs0(base, 0);

	for (int i = 0; i < SD_CMD0_RETRIES; i++) {
		r1 = sd_command(base, 0, 0);
		if (r1 == SD_R1_IDLE)
			break;
	}
	if (r1 == SD_R1_NO_RESP) return -1;
	if (r1 != SD_R1_IDLE)    return -2;

	r1 = sd_command(base, 8, 0x1AA);
	if (r1 != SD_R1_IDLE) return -3;
	uint32_t r7 = sd_read_response32(base);
	if ((r7 & 0xFFF) != 0x1AA) return -4;

	for (int i = 0; i < SD_ACMD41_RETRIES; i++) {
		r1 = sd_command(base, 55, 0);
		if (r1 != SD_R1_IDLE) return -5;
		r1 = sd_command(base, 41, 0x40000000);
		if (r1 == 0x00)
			break;
		if (r1 != SD_R1_IDLE) return -6;
	}
	if (r1 != 0x00) return -7;

	r1 = sd_command(base, 58, 0);
	uint32_t ocr = sd_read_response32(base);
	if (!(ocr & 0x40000000)) return -8;

	spi_set_clkdiv(base, 0);
	return 0;
}

static void
sd_deinit(uint32_t base)
{
	spi_cs0(base, 1);
	spi_set_clkdiv(base, 0xFF);
}

static int
sd_read_sector(uint32_t base, uint32_t lba, unsigned char *dst)
{
	unsigned char r1, tok;

	r1 = sd_command(base, 17, lba);
	if (r1 != 0x00)
		return -1;

	for (int i = 0; i < SD_DATA_RETRIES; i++) {
		tok = spi_transfer(base, 0xFF);
		if (tok == 0xFE)
			break;
	}
	if (tok != 0xFE)
		return -1;

	for (int i = 0; i < 512; i++)
		dst[i] = spi_transfer(base, 0xFF);

	spi_transfer(base, 0xFF);
	spi_transfer(base, 0xFF);

	return 0;
}

/* ── Boot data helpers ───────────────────────────────────────────── */

/*
 * Find the nth BTAG_DEVICE entry in the boot data list.
 */
static struct btag_device *
bd_find_device_nth(uint32_t bootdata, int nth)
{
	uint32_t p = bootdata + sizeof(struct bootdata_hdr);
	int count = 0;

	for (;;) {
		struct btag_hdr *h = (struct btag_hdr *)p;
		if (h->type == BTAG_END)
			return NULL;
		if (h->type == BTAG_DEVICE) {
			if (count == nth)
				return (struct btag_device *)p;
			count++;
		}
		p += h->size;
	}
}

/*
 * Find the first BTAG_BOOTDEV entry.
 */
static struct btag_bootdev *
bd_find_bootdev(uint32_t bootdata)
{
	uint32_t p = bootdata + sizeof(struct bootdata_hdr);

	for (;;) {
		struct btag_hdr *h = (struct btag_hdr *)p;
		if (h->type == BTAG_END)
			return NULL;
		if (h->type == BTAG_BOOTDEV)
			return (struct btag_bootdev *)p;
		p += h->size;
	}
}

/* ── Device state (one SD card, one SPI controller) ──────────────── */

static uint32_t sd_spi_base;	/* SPI controller MMIO base */
static uint32_t sd_part_lba;	/* LBA offset of boot partition */
static int      sd_is_open;

/* ── MBR partition lookup ────────────────────────────────────────── */

static int
mbr_find_fat32(uint32_t base, uint32_t *lba_start)
{
	unsigned char mbr[512];
	int rc;

	rc = sd_read_sector(base, 0, mbr);
	if (rc != 0)
		return -1;

	/* Check MBR signature */
	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return -2;

	/* Scan for first FAT32 partition */
	for (int i = 0; i < 4; i++) {
		const unsigned char *e = mbr + 446 + i * 16;
		unsigned char type = e[4];
		if (type == 0x0B || type == 0x0C) {
			*lba_start = (uint32_t)e[8]
			    | ((uint32_t)e[9] << 8)
			    | ((uint32_t)e[10] << 16)
			    | ((uint32_t)e[11] << 24);
			return 0;
		}
	}
	return -3;	/* no FAT32 partition found */
}

/* ── libsa device interface ──────────────────────────────────────── */

int
sdstrategy(void *devdata, int rw, daddr_t bn, size_t reqcnt,
    void *addr, size_t *cnt)
{
	unsigned char *p = addr;
	size_t done = 0;
	int rc;

	if (rw != F_READ) {
		*cnt = 0;
		return EROFS;
	}

	if (reqcnt & (DEV_BSIZE - 1)) {
		*cnt = 0;
		return EINVAL;
	}

	while (done < reqcnt) {
		rc = sd_read_sector(sd_spi_base,
		    sd_part_lba + (uint32_t)bn + (done / DEV_BSIZE), p);
		if (rc != 0) {
			*cnt = done;
			return EIO;
		}
		p += DEV_BSIZE;
		done += DEV_BSIZE;
	}

	*cnt = done;
	return 0;
}

int
sdopen(struct open_file *f, ...)
{
	if (sd_is_open)
		return EBUSY;

	sd_is_open = 1;
	return 0;
}

int
sdclose(struct open_file *f)
{
	sd_is_open = 0;
	return 0;
}

int
sdioctl(struct open_file *f, u_long cmd, void *data)
{
	return EIO;
}

/*
 * Set up the SD block device from boot data.
 * Called once at boot before any file operations.
 * Returns 0 on success.
 */
int
sd_boot_init(uint32_t bootdata)
{
	struct btag_bootdev *bdev;
	struct btag_device *dev;
	int rc;

	bdev = bd_find_bootdev(bootdata);
	if (bdev == NULL)
		return -1;

	dev = bd_find_device_nth(bootdata, bdev->dev_nth);
	if (dev == NULL)
		return -2;

	sd_spi_base = dev->base;

	rc = sd_init(sd_spi_base);
	if (rc != 0) {
		printf("SD init failed: %d\n", rc);
		return -3;
	}

	rc = mbr_find_fat32(sd_spi_base, &sd_part_lba);
	if (rc != 0) {
		printf("No FAT32 partition: %d\n", rc);
		sd_deinit(sd_spi_base);
		return -4;
	}

	printf("SD: SPI at 0x%x, FAT32 at LBA %d\n",
	    (unsigned)sd_spi_base, (unsigned)sd_part_lba);
	return 0;
}

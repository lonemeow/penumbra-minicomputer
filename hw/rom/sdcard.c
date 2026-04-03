/*
 * sdcard.c — SD card (SPI mode) driver for Penumbra boot ROM
 *
 * Fully stateless: each operation does init -> work -> deinit, so the
 * card can be swapped between commands and the next boot stage
 * inherits clean hardware state.
 */

#include "sdcard.h"
#include "spi.h"
#include "bootdata.h"
#include "console.h"
#include "util.h"

/* ── MBR partition table ──────────────────────────────────────────── */

const char *part_type_name(unsigned char type) {
    switch (type) {
    case PTYPE_EMPTY:   return "empty";
    case PTYPE_FAT12:   return "FAT12";
    case PTYPE_FAT16S:  return "FAT16 <32M";
    case PTYPE_FAT16:   return "FAT16";
    case PTYPE_FAT32:   return "FAT32";
    case PTYPE_FAT32L:  return "FAT32-LBA";
    case PTYPE_LINUX:   return "Linux";
    case PTYPE_FREEBSD: return "FreeBSD";
    case PTYPE_NETBSD:  return "NetBSD";
    case PTYPE_SWAP:    return "Linux swap";
    default:            return "unknown";
    }
}

/* ── SD SPI protocol ──────────────────────────────────────────────── */

/* R1 response bit masks */
#define SD_R1_IDLE       0x01
#define SD_R1_ERASE_RST  0x02
#define SD_R1_ILLEGAL    0x04
#define SD_R1_CRC_ERR    0x08
#define SD_R1_ERASE_SEQ  0x10
#define SD_R1_ADDR_ERR   0x20
#define SD_R1_PARAM_ERR  0x40
#define SD_R1_NO_RESP    0xFF

/* Timeout limits (iteration counts, not real time) */
#define SD_CMD0_RETRIES   20
#define SD_ACMD41_RETRIES 1500
#define SD_DATA_RETRIES   1000
#define SD_RESP_RETRIES   8

/*
 * Send an SD command (6 bytes) and return the R1 response.
 * Polls up to SD_RESP_RETRIES bytes waiting for bit 7 to clear.
 */
static unsigned char sd_command(uint32_t base, unsigned char cmd,
                                uint32_t arg) {
    spi_transfer(base, 0x40 | cmd);
    spi_transfer(base, (unsigned char)(arg >> 24));
    spi_transfer(base, (unsigned char)(arg >> 16));
    spi_transfer(base, (unsigned char)(arg >> 8));
    spi_transfer(base, (unsigned char)(arg));
    /* CRC — only CMD0 and CMD8 need valid CRC in SPI mode */
    if (cmd == 0)
        spi_transfer(base, 0x95);
    else if (cmd == 8)
        spi_transfer(base, 0x87);
    else
        spi_transfer(base, 0xFF);

    unsigned char r;
    for (int i = 0; i < SD_RESP_RETRIES; i++) {
        r = spi_transfer(base, 0xFF);
        if (!(r & 0x80))
            return r;
    }
    return r;
}

/* Read a 32-bit big-endian response (CMD8 R7 tail, CMD58 OCR, etc.) */
static uint32_t sd_read_response32(uint32_t base) {
    uint32_t val;
    val  = (uint32_t)spi_transfer(base, 0xFF) << 24;
    val |= (uint32_t)spi_transfer(base, 0xFF) << 16;
    val |= (uint32_t)spi_transfer(base, 0xFF) << 8;
    val |= (uint32_t)spi_transfer(base, 0xFF);
    return val;
}

int sd_init(uint32_t base) {
    unsigned char r1;

    /* Slow clock for init (<=400 kHz) */
    spi_set_clkdiv(base, 0xFF);

    /* 80+ clock cycles with CS deasserted (card power-up) */
    spi_cs0(base, 1);
    for (int i = 0; i < 20; i++)
        spi_transfer(base, 0xFF);

    spi_cs0(base, 0);

    /* CMD0 — go idle */
    for (int i = 0; i < SD_CMD0_RETRIES; i++) {
        r1 = sd_command(base, 0, 0);
        if (r1 == SD_R1_IDLE)
            break;
    }
    if (r1 == SD_R1_NO_RESP) { r1 = -1; goto fail; }
    if (r1 != SD_R1_IDLE)    { r1 = -2; goto fail; }

    /* CMD8 — send interface condition (voltage check) */
    r1 = sd_command(base, 8, 0x1AA);
    if (r1 != SD_R1_IDLE) { r1 = -3; goto fail; }
    uint32_t r7 = sd_read_response32(base);
    if ((r7 & 0xFFF) != 0x1AA) { r1 = -4; goto fail; }

    /* CMD55 + ACMD41 — app-specific init, wait for ready */
    for (int i = 0; i < SD_ACMD41_RETRIES; i++) {
        r1 = sd_command(base, 55, 0);
        if (r1 != SD_R1_IDLE) { r1 = -5; goto fail; }
        r1 = sd_command(base, 41, 0x40000000);
        if (r1 == 0x00)
            break;
        if (r1 != SD_R1_IDLE) { r1 = -6; goto fail; }
    }
    if (r1 != 0x00) { r1 = -7; goto fail; }

    /* CMD58 — read OCR, check SDHC (block addressing) */
    r1 = sd_command(base, 58, 0);
    uint32_t ocr = sd_read_response32(base);
    if (!(ocr & 0x40000000)) { r1 = -8; goto fail; }

    /* Switch to fast clock for data transfers */
    spi_set_clkdiv(base, 0);
    return 0;

fail:
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);
    return (int)(signed char)r1;
}

void sd_deinit(uint32_t base) {
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);
}

int sd_read_sector(uint32_t base, uint32_t lba, unsigned char *dst) {
    unsigned char r1 = sd_command(base, 17, lba);
    if (r1 != 0x00)
        return -1;

    /* Wait for data token (0xFE) */
    unsigned char tok;
    for (int i = 0; i < SD_DATA_RETRIES; i++) {
        tok = spi_transfer(base, 0xFF);
        if (tok == 0xFE)
            break;
    }
    if (tok != 0xFE)
        return -1;

    for (int i = 0; i < 512; i++)
        dst[i] = spi_transfer(base, 0xFF);

    /* Discard CRC16 */
    spi_transfer(base, 0xFF);
    spi_transfer(base, 0xFF);

    return 0;
}

int mbr_get_partition(uint32_t spi_base, int part,
                      uint32_t *lba_start, uint32_t *sector_count) {
    unsigned char mbr[512];

    int rc = sd_init(spi_base);
    if (rc != 0)
        return -1;

    rc = sd_read_sector(spi_base, 0, mbr);
    sd_deinit(spi_base);
    if (rc != 0)
        return -2;

    if (read_le16(mbr + MBR_SIG_OFFSET) != MBR_SIGNATURE)
        return -3;

    if (part < 1 || part > MBR_MAX_PARTS)
        return -4;

    const unsigned char *e = mbr + MBR_PART_OFFSET +
                             (part - 1) * MBR_PART_ENTRY_SIZE;
    unsigned char type = e[4];
    if (type == PTYPE_EMPTY)
        return -4;

    *lba_start    = read_le32(e + 8);
    *sector_count = read_le32(e + 12);
    return 0;
}

int sd_detect(uint32_t base) {
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
    spi_cs0(base, 1);
    spi_set_clkdiv(base, 0xFF);

    return r1 == SD_R1_IDLE;
}

int sd_probe(void) {
    int first = -1;
    for (int i = 0; ; i++) {
        struct btag_device *dev = bd_find_device_by_class(ACFG_CLASS_SD, i);
        if (!dev)
            break;
        console_printf("  sd:%d,0 ... ", i);
        if (sd_detect(dev->base)) {
            console_puts("card present\r\n");
            if (first < 0)
                first = i;
        } else {
            console_puts("empty\r\n");
        }
    }
    return first;
}

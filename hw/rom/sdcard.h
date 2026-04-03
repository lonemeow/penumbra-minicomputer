/*
 * sdcard.h — SD card (SPI mode) driver for Penumbra boot ROM
 *
 * Fully stateless: each operation does init -> work -> deinit, so the
 * card can be swapped between commands and the next boot stage
 * inherits clean hardware state.
 */

#ifndef SDCARD_H
#define SDCARD_H

#include "penumbra.h"

/* ── MBR partition table constants ────────────────────────────────── */

#define MBR_SIGNATURE       0xAA55
#define MBR_PART_OFFSET     446
#define MBR_PART_ENTRY_SIZE 16
#define MBR_SIG_OFFSET      510
#define MBR_MAX_PARTS       4

/* Partition type codes (subset) */
#define PTYPE_EMPTY    0x00
#define PTYPE_FAT12    0x01
#define PTYPE_FAT16S   0x04
#define PTYPE_FAT16    0x06
#define PTYPE_FAT32    0x0B
#define PTYPE_FAT32L   0x0C   /* FAT32 with LBA */
#define PTYPE_LINUX    0x83
#define PTYPE_FREEBSD  0xA5
#define PTYPE_NETBSD   0xA9
#define PTYPE_SWAP     0x82

/* Human-readable partition type name. */
const char *part_type_name(unsigned char type);

/* ── SD card operations ───────────────────────────────────────────── */

/*
 * Initialize SD card on the given SPI controller.
 * Leaves CS asserted and clock set fast on success.
 * Returns 0 on success, negative error code on failure:
 *   -1  CMD0 no response (empty slot)
 *   -2  CMD0 unexpected response
 *   -3  CMD8 rejected
 *   -4  CMD8 voltage/pattern mismatch
 *   -5  CMD55 rejected
 *   -6  ACMD41 rejected
 *   -7  ACMD41 timeout (card never left idle)
 *   -8  CMD58 not SDHC (no block addressing)
 */
int sd_init(uint32_t base);

/* Return the SPI controller to clean state: CS deasserted, slow clock. */
void sd_deinit(uint32_t base);

/*
 * Read one 512-byte sector from an already-initialized SD card.
 * Returns 0 on success, -1 on error.
 */
int sd_read_sector(uint32_t base, uint32_t lba, unsigned char *dst);

/*
 * Probe for SD card presence on a single controller.
 * Does CMD0 only — checks if a card responds.
 * Returns 1 if a card is present, 0 if empty slot.
 */
int sd_detect(uint32_t base);

/*
 * Probe all CLASS_SD devices and report which have cards.
 * Returns the SD controller index of the first card found, or -1.
 * (sd:X,Y naming — X is the Nth SD controller, not the global
 * device index.)
 */
int sd_probe(void);

/*
 * Read MBR from sector 0 and look up partition `part` (1-based).
 * On success, stores partition LBA start in *lba_start and sector
 * count in *sector_count.  Returns 0 on success, negative on error:
 *   -1  SD init failed
 *   -2  sector 0 read failed
 *   -3  bad MBR signature
 *   -4  partition empty or out of range
 */
int mbr_get_partition(uint32_t spi_base, int part,
                      uint32_t *lba_start, uint32_t *sector_count);

#endif /* SDCARD_H */

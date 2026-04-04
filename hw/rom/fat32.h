/*
 * fat32.h — Minimal read-only FAT32 filesystem for boot ROM
 *
 * Device-independent: takes a block-read callback so it works with
 * SD cards, NOR flash, or any future block device.  All state lives
 * in the caller-provided struct (no globals — ROM has no writable
 * data section).
 */

#ifndef FAT32_H
#define FAT32_H

#include "penumbra.h"

/*
 * Block read callback.
 *   lba — absolute sector number on the device
 *   dst — 512-byte destination buffer
 *   ctx — opaque context (e.g., SPI base address)
 * Returns 0 on success, non-zero on error.
 */
typedef int (*blk_read_fn)(uint32_t lba, unsigned char *dst, void *ctx);

/*
 * FAT32 mount context.  Populated by fat32_mount(), passed to all
 * other functions.  Lives on the caller's stack.
 */
struct fat32 {
    blk_read_fn  read;          /* block read callback */
    void        *read_ctx;      /* opaque context for callback */
    uint32_t     part_lba;      /* partition start LBA */
    uint32_t     fat_lba;       /* first FAT sector (absolute) */
    uint32_t     data_lba;      /* first data sector (absolute) */
    uint32_t     root_cluster;  /* root directory first cluster */
    unsigned     spc;           /* sectors per cluster */
};

/*
 * Mount a FAT32 partition.
 *   fs       — context to populate
 *   read     — block read callback
 *   ctx      — opaque context passed to read()
 *   part_lba — absolute LBA of the partition start (from MBR)
 *
 * Reads the BPB and populates fs.  Returns 0 on success:
 *   -1  read error on BPB sector
 *   -2  not a valid FAT32 volume (bad sector size, missing sig, etc.)
 */
int fat32_mount(struct fat32 *fs, blk_read_fn read, void *ctx,
                uint32_t part_lba);

/*
 * Look up a file by name in the root directory.
 *   fs         — mounted FAT32 context
 *   name       — filename to find (matched case-insensitively,
 *                supports "name.ext" or raw 8.3 padded form)
 *   out_cluster — receives the file's first cluster
 *   out_size    — receives the file size in bytes
 *
 * Only searches the root directory (no subdirectory traversal).
 * Returns 0 if found, -1 if not found or on I/O error.
 */
int fat32_find_root(struct fat32 *fs, const char *name,
                    uint32_t *out_cluster, uint32_t *out_size);

/*
 * Read an entire file into memory.
 *   fs      — mounted FAT32 context
 *   cluster — first cluster (from fat32_find_root)
 *   dst     — destination buffer (must be large enough)
 *   size    — file size in bytes (from fat32_find_root)
 *
 * Follows the cluster chain in the FAT.  Returns 0 on success,
 * -1 on read error or broken chain.
 */
int fat32_read_file(struct fat32 *fs, uint32_t cluster,
                    unsigned char *dst, uint32_t size);

#endif /* FAT32_H */

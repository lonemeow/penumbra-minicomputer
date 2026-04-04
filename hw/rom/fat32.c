/*
 * fat32.c — Minimal read-only FAT32 filesystem for boot ROM
 *
 * Supports only what the boot ROM needs:
 *   - Mount a FAT32 partition (parse BPB)
 *   - Find a file in the root directory by 8.3 name
 *   - Read a file by following its cluster chain
 *
 * No subdirectory traversal, no long filenames, no write support.
 * All state in caller-provided struct fat32 (no globals).
 */

#include "fat32.h"
#include "util.h"

/* ── BPB field offsets ──────────────────────────────────────────────── */

#define BPB_BYTES_PER_SECTOR    11
#define BPB_SECTORS_PER_CLUSTER 13
#define BPB_RESERVED_SECTORS    14
#define BPB_NUM_FATS            16
#define BPB_ROOT_ENTRY_COUNT    17
#define BPB_TOTAL_SECTORS_16    19
#define BPB_FAT_SIZE_16         22
#define BPB_FAT_SIZE_32         36
#define BPB_ROOT_CLUSTER        44
#define BPB_SIGNATURE           510

/* ── FAT32 constants ────────────────────────────────────────────────── */

#define FAT32_EOC       0x0FFFFFF8  /* end-of-chain marker (>= this) */
#define DIR_ENTRY_SIZE  32
#define ENTRIES_PER_SECTOR (512 / DIR_ENTRY_SIZE)

#define ATTR_LONG_NAME  0x0F
#define ATTR_VOLUME_ID  0x08
#define DIR_FREE        0xE5
#define DIR_END         0x00

/* ── Helpers ────────────────────────────────────────────────────────── */

/*
 * Convert a cluster number to its first absolute LBA.
 * Clusters are numbered from 2 (cluster 0 and 1 don't exist on disk).
 */
static uint32_t cluster_to_lba(const struct fat32 *fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2) * fs->spc;
}

/*
 * Read the next cluster number from the FAT.
 * Returns the next cluster, or 0 on read error.
 */
static uint32_t fat_next_cluster(const struct fat32 *fs, uint32_t cluster)
{
    /*
     * Each FAT32 entry is 4 bytes.  128 entries per 512-byte sector.
     * FAT sector = fat_lba + (cluster * 4) / 512
     * Offset within sector = (cluster * 4) % 512
     */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_lba + (fat_offset / 512);
    uint32_t fat_index  = fat_offset % 512;

    unsigned char buf[512];
    if (fs->read(fat_sector, buf, fs->read_ctx) != 0)
        return 0;

    return read_le32(buf + fat_index) & 0x0FFFFFFF;
}

/*
 * Convert a "name.ext" string to FAT 8.3 format (11 bytes, space-padded).
 * Case-insensitive: converts to uppercase.
 */
static void make_8_3(const char *name, unsigned char *out)
{
    int i;

    /* Fill with spaces */
    for (i = 0; i < 11; i++)
        out[i] = ' ';

    /* Copy name part (up to 8 chars, stop at dot or end) */
    i = 0;
    while (*name && *name != '.' && i < 8) {
        unsigned char c = *name++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i++] = c;
    }

    /* Skip to dot */
    while (*name && *name != '.')
        name++;
    if (*name == '.')
        name++;

    /* Copy extension (up to 3 chars) */
    i = 8;
    while (*name && i < 11) {
        unsigned char c = *name++;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i++] = c;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int
fat32_mount(struct fat32 *fs, blk_read_fn read, void *ctx, uint32_t part_lba)
{
    unsigned char bpb[512];

    fs->read = read;
    fs->read_ctx = ctx;
    fs->part_lba = part_lba;

    if (read(part_lba, bpb, ctx) != 0) {
        return -1;
    }

    unsigned short boot_signature = read_le16(bpb + BPB_SIGNATURE);
    unsigned short bytes_per_sector = read_le16(bpb + BPB_BYTES_PER_SECTOR);
    if (boot_signature != 0xAA55 || bytes_per_sector != 512) {
        return -2;
    }

    unsigned int reserved_sectors = read_le16(bpb + BPB_RESERVED_SECTORS);
    unsigned int num_fats = bpb[BPB_NUM_FATS];
    uint32_t fat_size = read_le32(bpb + BPB_FAT_SIZE_32);

    fs->spc = bpb[BPB_SECTORS_PER_CLUSTER];
    fs->root_cluster = read_le32(bpb + BPB_ROOT_CLUSTER);
    fs->fat_lba = part_lba + reserved_sectors;
    fs->data_lba = fs->fat_lba + (num_fats * fat_size);

    return 0;
}

int
fat32_find_root(struct fat32 *fs, const char *name,
                uint32_t *out_cluster, uint32_t *out_size)
{
    unsigned char dir[512];
    unsigned char name83[11];
    uint32_t cluster = fs->root_cluster;

    make_8_3(name, name83);

    /* Walk the root directory cluster chain */
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        unsigned s;

        for (s = 0; s < fs->spc; s++) {
            int e;

            if (fs->read(lba + s, dir, fs->read_ctx) != 0)
                return -1;

            for (e = 0; e < ENTRIES_PER_SECTOR; e++) {
                unsigned char *ent = dir + e * DIR_ENTRY_SIZE;

                if (ent[0] == DIR_END)
                    return -1;  /* no more entries */
                if (ent[0] == DIR_FREE)
                    continue;
                if (ent[11] & (ATTR_LONG_NAME | ATTR_VOLUME_ID))
                    continue;

                /* Compare 8.3 name */
                int match = 1;
                for (int j = 0; j < 11; j++) {
                    unsigned char c = ent[j];
                    if (c >= 'a' && c <= 'z')
                        c -= 32;
                    if (c != name83[j]) {
                        match = 0;
                        break;
                    }
                }

                if (match) {
                    uint32_t cl_hi = read_le16(ent + 20);
                    uint32_t cl_lo = read_le16(ent + 26);
                    *out_cluster = (cl_hi << 16) | cl_lo;
                    *out_size = read_le32(ent + 28);
                    return 0;
                }
            }
        }

        cluster = fat_next_cluster(fs, cluster);
    }

    return -1;
}

int
fat32_read_file(struct fat32 *fs, uint32_t cluster,
                unsigned char *dst, uint32_t size)
{
    uint32_t remaining = size;

    while (remaining > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint32_t cluster_bytes = (uint32_t)fs->spc * 512;
        uint32_t to_read = remaining < cluster_bytes
                         ? remaining : cluster_bytes;

        /* Read whole sectors from this cluster */
        uint32_t sectors = (to_read + 511) / 512;
        for (uint32_t s = 0; s < sectors; s++) {
            if (fs->read(lba + s, dst, fs->read_ctx) != 0)
                return -1;
            dst += 512;
        }

        remaining -= to_read;
        cluster = fat_next_cluster(fs, cluster);
    }

    if (remaining > 0)
        return -1;  /* cluster chain ended early */

    return 0;
}

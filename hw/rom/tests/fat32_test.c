/*
 * fat32_test.c — host-side unit test for the boot ROM's FAT32 reader
 *
 * fat32.c reaches the disk only through a blk_read_fn callback, so it
 * runs unmodified on the host against a synthesized image.  The
 * callback counts reads per region, which is what makes the FAT
 * cache's contract testable: a chain walk must touch each FAT sector
 * at most once, no matter how many clusters it covers.
 *
 * The image is built here rather than read from build/ so the test
 * needs no external tools and no prior build products.
 *
 * Build and run: make -C hw/rom test
 */

#include "../fat32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Synthesized volume geometry ────────────────────────────────────── */

#define SECTOR          512
#define SPC             2      /* 1 KB clusters, as makefs picks for the SD images */
#define PART_LBA        2048   /* where the ROM's images place the FAT partition */
#define RESERVED        32
#define NUM_FATS        2
#define FAT_SECTORS     4
#define TOTAL_CLUSTERS  300

#define FAT_LBA         (PART_LBA + RESERVED)
#define DATA_LBA        (FAT_LBA + NUM_FATS * FAT_SECTORS)
#define TOTAL_SECTORS   (DATA_LBA + TOTAL_CLUSTERS * SPC)

#define ROOT_CLUSTER    2
#define FILE_CLUSTER    3
/*
 * 198 clusters starting at 3 runs the chain to cluster 200, crossing
 * the 128-entries-per-sector boundary — so the walk needs a second FAT
 * sector and the cache must refill rather than serve a stale hit.
 */
#define FILE_CLUSTERS   198
#define FILE_SIZE       (197 * 1024 + 500)   /* partial last cluster */
#define FILE_NAME       "PENBOOT.ELF"

#define FAT_ENTRIES_PER_SECTOR (SECTOR / 4)
#define EOC             0x0FFFFFFF

/* ── Instrumented block device ──────────────────────────────────────── */

struct blkdev {
    unsigned char *image;

    unsigned long fat_reads;    /* reads landing in the first FAT */
    unsigned long data_reads;   /* reads landing in the data region */
    unsigned long other_reads;  /* BPB and anything else */
    int fat_sector_seen[FAT_SECTORS];

    uint32_t fail_lba;          /* when fail_enabled, reads of this LBA fail */
    int fail_enabled;
};

/*
 * Failing reads scribble the destination first.  A cache that records
 * a sector as present before confirming the read succeeded would then
 * serve this garbage on the retry, which is exactly what the error
 * test is looking for.
 */
static int blk_read(uint32_t lba, unsigned char *dst, void *ctx)
{
    struct blkdev *dev = ctx;

    if (dev->fail_enabled && lba == dev->fail_lba) {
        memset(dst, 0xA5, SECTOR);
        return -1;
    }

    if (lba >= TOTAL_SECTORS)
        return -1;

    if (lba >= FAT_LBA && lba < FAT_LBA + FAT_SECTORS) {
        dev->fat_reads++;
        dev->fat_sector_seen[lba - FAT_LBA] = 1;
    } else if (lba >= DATA_LBA) {
        dev->data_reads++;
    } else {
        dev->other_reads++;
    }

    memcpy(dst, dev->image + (size_t)lba * SECTOR, SECTOR);
    return 0;
}

static void reset_counters(struct blkdev *dev)
{
    dev->fat_reads = 0;
    dev->data_reads = 0;
    dev->other_reads = 0;
    memset(dev->fat_sector_seen, 0, sizeof(dev->fat_sector_seen));
}

static int distinct_fat_sectors(const struct blkdev *dev)
{
    int n = 0;
    for (int i = 0; i < FAT_SECTORS; i++)
        n += dev->fat_sector_seen[i];
    return n;
}

/* ── Image construction ─────────────────────────────────────────────── */

static void put_le16(unsigned char *p, unsigned v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/* Position-dependent so a chain that jumps to the wrong cluster shows up. */
static unsigned char file_byte(uint32_t i)
{
    return (unsigned char)((i >> 8) ^ (i & 0xFF) ^ 0x5A);
}

static unsigned char *build_image(void)
{
    unsigned char *img = calloc(TOTAL_SECTORS, SECTOR);
    if (!img) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    /* BPB */
    unsigned char *bpb = img + (size_t)PART_LBA * SECTOR;
    put_le16(bpb + 11, SECTOR);
    bpb[13] = SPC;
    put_le16(bpb + 14, RESERVED);
    bpb[16] = NUM_FATS;
    put_le32(bpb + 36, FAT_SECTORS);
    put_le32(bpb + 44, ROOT_CLUSTER);
    put_le16(bpb + 510, 0xAA55);

    /* FAT: root dir is one cluster, the file chains 3 → 200 */
    unsigned char *fat = img + (size_t)FAT_LBA * SECTOR;
    put_le32(fat + 0 * 4, 0x0FFFFFF8);
    put_le32(fat + 1 * 4, EOC);
    put_le32(fat + ROOT_CLUSTER * 4, EOC);
    for (uint32_t i = 0; i < FILE_CLUSTERS; i++) {
        uint32_t cl = FILE_CLUSTER + i;
        put_le32(fat + cl * 4,
                 (i == FILE_CLUSTERS - 1) ? EOC : cl + 1);
    }

    /* Root directory: one 8.3 entry, then end-of-directory */
    unsigned char *root = img + (size_t)DATA_LBA * SECTOR;
    memcpy(root, "PENBOOT ELF", 11);
    root[11] = 0x20;                      /* archive */
    put_le16(root + 20, FILE_CLUSTER >> 16);
    put_le16(root + 26, FILE_CLUSTER & 0xFFFF);
    put_le32(root + 28, FILE_SIZE);
    root[32] = 0x00;                      /* DIR_END */

    /* File payload */
    unsigned char *data = img +
        (size_t)(DATA_LBA + (FILE_CLUSTER - 2) * SPC) * SECTOR;
    for (uint32_t i = 0; i < FILE_SIZE; i++)
        data[i] = file_byte(i);

    return img;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int failures;

#define CHECK(cond, ...) do {                          \
    if (!(cond)) {                                     \
        printf("  FAIL: ");                            \
        printf(__VA_ARGS__);                           \
        printf("\n");                                  \
        failures++;                                    \
    }                                                  \
} while (0)

/*
 * The whole file reads back byte-exact, and the chain walk costs one
 * read per distinct FAT sector rather than one per cluster.
 */
static void test_chain_walk_reads_each_fat_sector_once(struct blkdev *dev)
{
    struct fat32 fs;
    uint32_t cluster, size;

    printf("chain walk reads each FAT sector once\n");
    reset_counters(dev);

    CHECK(fat32_mount(&fs, blk_read, dev, PART_LBA) == 0, "mount failed");
    CHECK(fat32_find_root(&fs, FILE_NAME, &cluster, &size) == 0,
          "%s not found", FILE_NAME);
    CHECK(cluster == FILE_CLUSTER, "first cluster %u, want %u",
          cluster, FILE_CLUSTER);
    CHECK(size == FILE_SIZE, "size %u, want %u", size, (unsigned)FILE_SIZE);

    /* Whole sectors land in the buffer, so round up. */
    unsigned char *buf = calloc(FILE_CLUSTERS * SPC, SECTOR);
    CHECK(fat32_read_file(&fs, cluster, buf, size) == 0, "read failed");

    uint32_t bad = 0xFFFFFFFF;
    for (uint32_t i = 0; i < FILE_SIZE; i++) {
        if (buf[i] != file_byte(i)) { bad = i; break; }
    }
    CHECK(bad == 0xFFFFFFFF, "content differs at byte %u", bad);

    /*
     * Clusters 3..200 span FAT sectors 0 and 1, so the walk costs two
     * reads no matter how many clusters it crosses.
     */
    CHECK(distinct_fat_sectors(dev) == 2, "touched %d FAT sectors, want 2",
          distinct_fat_sectors(dev));
    CHECK(dev->fat_reads == 2, "%lu FAT reads, want 2 (one per distinct sector)",
          dev->fat_reads);

    /*
     * One root-dir sector, then every sector the file actually
     * occupies — the reader stops at the file's last byte, so the
     * partial final cluster costs one sector rather than SPC.
     */
    const int want_data = 1 + (FILE_SIZE + SECTOR - 1) / SECTOR;
    CHECK(dev->data_reads == (unsigned long)want_data,
          "%lu data reads, want %d", dev->data_reads, want_data);
    CHECK(dev->other_reads == 1, "%lu non-FAT non-data reads, want 1 (the BPB)",
          dev->other_reads);

    free(buf);
}

/*
 * A FAT sector that fails to read must not leave anything behind for
 * the next attempt to hit.
 */
static void test_failed_fat_read_is_not_cached(struct blkdev *dev)
{
    struct fat32 fs;
    uint32_t cluster, size;

    printf("failed FAT read is not cached\n");
    reset_counters(dev);

    CHECK(fat32_mount(&fs, blk_read, dev, PART_LBA) == 0, "mount failed");
    CHECK(fat32_find_root(&fs, FILE_NAME, &cluster, &size) == 0,
          "%s not found", FILE_NAME);

    unsigned char *buf = calloc(FILE_CLUSTERS * SPC, SECTOR);

    /* Break the second FAT sector: the chain dies where it crosses over. */
    dev->fail_lba = FAT_LBA + 1;
    dev->fail_enabled = 1;
    CHECK(fat32_read_file(&fs, cluster, buf, size) == -1,
          "read succeeded despite a failing FAT sector");

    /* Same mount, same struct — a stale cache entry would surface here. */
    dev->fail_enabled = 0;
    memset(buf, 0, (size_t)FILE_CLUSTERS * SPC * SECTOR);
    CHECK(fat32_read_file(&fs, cluster, buf, size) == 0,
          "retry after a transient FAT read error failed");

    uint32_t bad = 0xFFFFFFFF;
    for (uint32_t i = 0; i < FILE_SIZE; i++) {
        if (buf[i] != file_byte(i)) { bad = i; break; }
    }
    CHECK(bad == 0xFFFFFFFF, "retry content differs at byte %u", bad);

    free(buf);
}

int main(void)
{
    struct blkdev dev;
    memset(&dev, 0, sizeof(dev));
    dev.image = build_image();

    test_chain_walk_reads_each_fat_sector_once(&dev);
    test_failed_fat_read_is_not_cached(&dev);

    free(dev.image);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}

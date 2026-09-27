/*
 * util.c — Utility functions for Penumbra boot ROM
 *
 * Unaligned LE reads, human-readable size formatting, etc.
 */

#include "util.h"
#include "libc.h"

unsigned int read_le16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Format a byte count as a human-readable size (e.g. "1.5GB"): the
 * largest unit the count has a whole one of, and one decimal digit,
 * truncated. Unit i is 2^(10 i) bytes, so it divides by shifting: the
 * ROM has no 64-bit division, and the count is 64 bits because a disk
 * partition can pass 4 GB.
 */
void humanize_size(unsigned long long bytes, char *buf, unsigned long bufsize) {
    static const char * const suffixes[] = {
        "B", "kB", "MB", "GB", "TB"
    };
    enum { LARGEST_UNIT = 4 };

    int idx = 0;
    for (int i = LARGEST_UNIT; i >= 1; i--) {
        if (bytes >> (10 * i)) {
            idx = i;
            break;
        }
    }

    unsigned int       shift = 10 * idx;
    unsigned long long whole = bytes >> shift;
    unsigned long long rem   = bytes - (whole << shift);
    unsigned long long frac  = (rem * 10) >> shift;

    if (idx == 0 || frac == 0)
        snprintf(buf, bufsize, "%u%s", (unsigned int)whole, suffixes[idx]);
    else
        snprintf(buf, bufsize, "%u.%u%s", (unsigned int)whole,
                 (unsigned int)frac, suffixes[idx]);
}

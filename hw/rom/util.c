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
 * Format a byte count as a human-readable size (e.g. "1.5GB").
 * Uses the largest unit where the integer part is >= 1, with one
 * decimal digit via integer fixed-point: frac = (rem * 10) / div.
 * No floating point needed.
 */
void humanize_size(unsigned long bytes, char *buf, unsigned long bufsize) {
    static const char * const suffixes[] = {
        "B", "kB", "MB", "GB"
    };
    static const unsigned long divisors[] = {
        1, 1024, 1024 * 1024, 1024 * 1024 * 1024
    };

    /* Pick largest unit where whole part >= 1 */
    int idx = 0;
    for (int i = 3; i >= 1; i--) {
        if (bytes >= divisors[i]) {
            idx = i;
            break;
        }
    }

    unsigned long whole = bytes / divisors[idx];
    unsigned long frac  = (bytes % divisors[idx]) * 10 / divisors[idx];

    if (idx == 0 || frac == 0)
        snprintf(buf, bufsize, "%u%s", (unsigned int)whole, suffixes[idx]);
    else
        snprintf(buf, bufsize, "%u.%u%s", (unsigned int)whole,
                 (unsigned int)frac, suffixes[idx]);
}

/*
 * util.h — Utility functions for Penumbra boot ROM
 */

#ifndef UTIL_H
#define UTIL_H

#include "penumbra.h"

/* Read a little-endian 16-bit value from unaligned memory. */
unsigned int read_le16(const unsigned char *p);

/* Read a little-endian 32-bit value from unaligned memory. */
uint32_t read_le32(const unsigned char *p);

/*
 * Format a byte count as a human-readable size (e.g. "1.5GB"), one
 * decimal digit, truncated. 64 bits: a partition can pass 4 GB.
 */
void humanize_size(unsigned long long bytes, char *buf, unsigned long bufsize);

#endif /* UTIL_H */

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
 * Format a byte count as a human-readable size (e.g. "1.5GB").
 * Uses integer fixed-point — no floating point needed.
 */
void humanize_size(unsigned long bytes, char *buf, unsigned long bufsize);

#endif /* UTIL_H */

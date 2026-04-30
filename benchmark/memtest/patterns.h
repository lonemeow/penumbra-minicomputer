/*
 * patterns.h — Memory test pattern API
 *
 * Each pattern function exercises a distinct failure class:
 *
 *   round_trip       1-word smoke test — controller reachable at all?
 *   walking_one      Stuck-at-zero bits, DQ-line shorts.
 *   walking_zero     Stuck-at-one bits, DQ-line shorts (inverse).
 *   address_as_data  Address-line faults / bank-row-col bit swaps.
 *   complement       Write-then-overwrite ordering (catches DQM stuck).
 *   sub_word         STB/STH/LDB/LDH lane positioning + byte_en path.
 *
 * All pattern functions share the same signature so memtest's
 * top-level driver can iterate over a table.  They write through
 * the tested word range, then read back and verify.  On the first
 * mismatch, they print a diagnostic and return non-zero; the
 * caller may continue with the next pattern or stop, per policy.
 *
 * Conventions:
 *   begin / end are uint32_t physical addresses (caller already
 *     reserved them — no allocator); end is exclusive; both are
 *     4-byte aligned for word-stride patterns.
 *   Returns 0 on success, non-zero on failure (also prints).
 */

#ifndef MEMTEST_PATTERNS_H
#define MEMTEST_PATTERNS_H

#include "bench.h"

typedef int (*pattern_fn)(uint32_t begin, uint32_t end);

struct pattern_entry {
    const char *name;
    pattern_fn  fn;
};

extern const struct pattern_entry pattern_table[];
extern const unsigned             pattern_table_len;

int pat_round_trip      (uint32_t begin, uint32_t end);
int pat_walking_one     (uint32_t begin, uint32_t end);
int pat_walking_zero    (uint32_t begin, uint32_t end);
int pat_address_as_data (uint32_t begin, uint32_t end);
int pat_complement      (uint32_t begin, uint32_t end);
int pat_sub_word        (uint32_t begin, uint32_t end);

/* Common diagnostic — prints "FAIL <pattern>: addr=... exp=... got=...". */
void mt_report_fail(const char *pat, uint32_t addr,
                    uint32_t expected, uint32_t got);

#endif

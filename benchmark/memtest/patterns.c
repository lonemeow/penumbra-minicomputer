/*
 * patterns.c — Memory test patterns.
 *
 * Each pattern is a self-contained pass over [begin, end).  All
 * memory accesses are through `volatile` pointers so the compiler
 * doesn't fold writes-then-reads into constants.  Each pattern
 * stops on the first mismatch and reports via mt_report_fail —
 * we want to know *what* failed and *where*, not just "something".
 */

#include "patterns.h"

/* Sub-word access types — bench.h only exposes uint32_t. */
typedef unsigned char  u8;
typedef unsigned short u16;

/* ── pat_round_trip ──────────────────────────────────────────────
 * Smoke test: one word, two values, two reads.  If this fails we
 * stop bothering with the rest — controller is unreachable.
 */
int pat_round_trip(uint32_t begin, uint32_t end) {
    volatile uint32_t *p = (volatile uint32_t *)begin;
    (void)end;

    *p = 0x12345678;
    if (*p != 0x12345678) {
        mt_report_fail("round_trip", begin, 0x12345678, *p);
        return 1;
    }
    *p = 0xDEADBEEF;
    if (*p != 0xDEADBEEF) {
        mt_report_fail("round_trip", begin, 0xDEADBEEF, *p);
        return 1;
    }
    return 0;
}

/* ── pat_walking_one ─────────────────────────────────────────────
 * For each bit position b in 0..31:
 *   write (1 << b) to every word in the window
 *   read every word, verify
 * Catches stuck-at-0 bits and shorts where a 1 on bit b also
 * appears on some other bit.
 *
 * "Across-region then verify" instead of "per-word inner loop"
 * because cache-line burst-fill makes the across-region form
 * dramatically faster: writes group into bursts, reads hit the
 * cache when the line is freshly written.
 */
int pat_walking_one(uint32_t begin, uint32_t end) {
    for (int b = 0; b < 32; b++) {
        uint32_t pat = 1u << b;
        for (uint32_t a = begin; a < end; a += 4)
            *(volatile uint32_t *)a = pat;
        for (uint32_t a = begin; a < end; a += 4) {
            uint32_t got = *(volatile uint32_t *)a;
            if (got != pat) {
                mt_report_fail("walking_one", a, pat, got);
                return 1;
            }
        }
        /* Liveness dot — one per bit position (~1 Hz on hardware
         * for the default 8 MiB window). */
        bench_putchar('.');
    }
    return 0;
}

/* ── pat_walking_zero ────────────────────────────────────────────
 * Same shape as walking_one but with the inverse pattern: every
 * bit *except* b is 1.  Catches stuck-at-1 bits.
 */
int pat_walking_zero(uint32_t begin, uint32_t end) {
    for (int b = 0; b < 32; b++) {
        uint32_t pat = ~(1u << b);
        for (uint32_t a = begin; a < end; a += 4)
            *(volatile uint32_t *)a = pat;
        for (uint32_t a = begin; a < end; a += 4) {
            uint32_t got = *(volatile uint32_t *)a;
            if (got != pat) {
                mt_report_fail("walking_zero", a, pat, got);
                return 1;
            }
        }
        bench_putchar('.');
    }
    return 0;
}

/* ── pat_address_as_data ─────────────────────────────────────────
 */
int pat_address_as_data(uint32_t begin, uint32_t end) {
    for (uint32_t a = begin; a < end; a += 4)
        *(volatile uint32_t *)a = a;
    for (uint32_t a = begin; a < end; a += 4) {
        uint32_t got = *(volatile uint32_t *)a;
        if (got != a) {
            mt_report_fail("address_as_data", a, a, got);
            return 1;
        }
    }

    for (uint32_t a = begin; a < end; a += 4)
        *(volatile uint32_t *)a = ~a;
    for (uint32_t a = begin; a < end; a += 4) {
        uint32_t exp = ~a;
        uint32_t got = *(volatile uint32_t *)a;
        if (got != exp) {
            mt_report_fail("address_as_data_c", a, exp, got);
            return 1;
        }
    }

    return 0;
}

/* ── pat_complement ──────────────────────────────────────────────
 * Two passes:
 *   1. Write 0xA5A5A5A5 everywhere, verify.
 *   2. Overwrite with 0x5A5A5A5A, verify.
 * If DQM is stuck at "all-mask" (writes silently dropped), pass 1
 * passes (we never wrote — but the stale data happens to match
 * only by luck of init), but pass 2 must observe the *new* value
 * — and that's where stuck-DQM shows up.
 */
int pat_complement(uint32_t begin, uint32_t end) {
    const uint32_t pat_a = 0xA5A5A5A5u;
    const uint32_t pat_b = 0x5A5A5A5Au;

    for (uint32_t a = begin; a < end; a += 4)
        *(volatile uint32_t *)a = pat_a;
    for (uint32_t a = begin; a < end; a += 4) {
        uint32_t got = *(volatile uint32_t *)a;
        if (got != pat_a) {
            mt_report_fail("complement(A5)", a, pat_a, got);
            return 1;
        }
    }

    for (uint32_t a = begin; a < end; a += 4)
        *(volatile uint32_t *)a = pat_b;
    for (uint32_t a = begin; a < end; a += 4) {
        uint32_t got = *(volatile uint32_t *)a;
        if (got != pat_b) {
            mt_report_fail("complement(5A)", a, pat_b, got);
            return 1;
        }
    }
    return 0;
}

/* ── pat_sub_word ────────────────────────────────────────────────
 *
 * Verifies STB/STH/LDB/LDH lane positioning end-to-end through
 * cache → bus_adapter → CDC → controller → PHY DQM.  This is the
 * path most likely to be broken by a refactor of the byte_en
 * pipeline.
 *
 * For each test address:
 *   • Reset the word to 0xDEADBEEF.
 *   • STB 0xAA at each of the four byte offsets, expecting the
 *     final word value to have just that one byte replaced.
 *   • STH 0xCAFE at offsets 0 and 2, expecting two halves replaced.
 *   • Write 0x44332211, then LDB at each byte offset and LDH at
 *     0 and 2 to verify *read* lane extraction (LE convention:
 *     byte at addr+0 ↔ bits [7:0]).
 *
 * Stride = 256 bytes so we hit a variety of column/row/bank bits
 * in the SDRAM controller's address mapping.
 */
int pat_sub_word(uint32_t begin, uint32_t end) {
    /* Each iteration is independent — keep the inner code small
     * by pre-computing the four expected post-STB values once. */
    const uint32_t init = 0xDEADBEEFu;
    const uint32_t exp_b0 = 0xDEADBEAAu;   /* byte 0 := 0xAA */
    const uint32_t exp_b1 = 0xDEADAAEFu;   /* byte 1 := 0xAA */
    const uint32_t exp_b2 = 0xDEAABEEFu;   /* byte 2 := 0xAA */
    const uint32_t exp_b3 = 0xAAADBEEFu;   /* byte 3 := 0xAA */
    const uint32_t exp_h0 = 0xDEADCAFEu;   /* halfword 0 := 0xCAFE */
    const uint32_t exp_h1 = 0xCAFEBEEFu;   /* halfword 1 := 0xCAFE */

    for (uint32_t a = begin; a + 4 <= end; a += 256) {
        volatile uint32_t *pw = (volatile uint32_t *)a;
        volatile u8       *pb = (volatile u8       *)a;
        volatile u16      *ph = (volatile u16      *)a;

        /* ── byte writes ── */
        *pw = init; *(pb + 0) = 0xAA;
        if (*pw != exp_b0) {
            mt_report_fail("sub_word STB+0", a, exp_b0, *pw); return 1;
        }
        *pw = init; *(pb + 1) = 0xAA;
        if (*pw != exp_b1) {
            mt_report_fail("sub_word STB+1", a, exp_b1, *pw); return 1;
        }
        *pw = init; *(pb + 2) = 0xAA;
        if (*pw != exp_b2) {
            mt_report_fail("sub_word STB+2", a, exp_b2, *pw); return 1;
        }
        *pw = init; *(pb + 3) = 0xAA;
        if (*pw != exp_b3) {
            mt_report_fail("sub_word STB+3", a, exp_b3, *pw); return 1;
        }

        /* ── halfword writes ── */
        *pw = init; *(ph + 0) = 0xCAFE;
        if (*pw != exp_h0) {
            mt_report_fail("sub_word STH+0", a, exp_h0, *pw); return 1;
        }
        *pw = init; *(ph + 1) = 0xCAFE;
        if (*pw != exp_h1) {
            mt_report_fail("sub_word STH+2", a, exp_h1, *pw); return 1;
        }

        /* ── byte reads (LE: byte i ↔ bits [8i+7:8i]) ── */
        *pw = 0x44332211u;
        if (*(pb + 0) != 0x11) { mt_report_fail("sub_word LDB+0", a, 0x11, *(pb + 0)); return 1; }
        if (*(pb + 1) != 0x22) { mt_report_fail("sub_word LDB+1", a, 0x22, *(pb + 1)); return 1; }
        if (*(pb + 2) != 0x33) { mt_report_fail("sub_word LDB+2", a, 0x33, *(pb + 2)); return 1; }
        if (*(pb + 3) != 0x44) { mt_report_fail("sub_word LDB+3", a, 0x44, *(pb + 3)); return 1; }

        /* ── halfword reads ── */
        if (*(ph + 0) != 0x2211) { mt_report_fail("sub_word LDH+0", a, 0x2211, *(ph + 0)); return 1; }
        if (*(ph + 1) != 0x4433) { mt_report_fail("sub_word LDH+2", a, 0x4433, *(ph + 1)); return 1; }
    }
    return 0;
}

/* ── Pattern table ───────────────────────────────────────────────
 * Order matters: cheapest sanity check first, most diagnostic
 * (address-line) last so a deeper bug masks fewer earlier passes.
 */
const struct pattern_entry pattern_table[] = {
    { "round_trip      ", pat_round_trip      },
    { "walking_one     ", pat_walking_one     },
    { "walking_zero    ", pat_walking_zero    },
    { "complement      ", pat_complement      },
    { "sub_word        ", pat_sub_word        },
    { "address_as_data ", pat_address_as_data },
};

const unsigned pattern_table_len =
    sizeof(pattern_table) / sizeof(pattern_table[0]);

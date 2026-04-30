/*
 * entry.c — bench_main wrapper for the memory tester.
 *
 * Walks the bootdata tag list to find the first ACFG_CLASS_MEMORY
 * device, picks a test window that doesn't overlap the program /
 * stack, and runs each pattern over the window.  Each pattern
 * reports start/end and pass/fail.  On any failure we keep going
 * (so the user gets the full picture for one pass) and return a
 * non-zero exit count at the end.
 *
 * Test window policy:
 *   • Reserve the first MEMTEST_RESERVED_LO bytes of RAM for our own
 *     code+data+stack (we live there — see bench.ld + crt0.S, stack
 *     at 0x100000 = 1 MB).
 *   • Cap the window at MEMTEST_MAX_BYTES so we don't spend hours
 *     walking 32 MB on every pattern by default.  Override with
 *     +full=1 plusarg style, or just edit the constant.
 *
 * Bootdata: BTAG_DEVICE entries with cls == ACFG_CLASS_MEMORY carry
 * the discovered RAM region (base, dev_size).  See hw/rom/bootdata.h.
 *
 * Cache policy: memtest disables both caches at entry.  crt0.S enables
 * them by default for performance benchmarks, but a memory *correctness*
 * tester needs every read to actually round-trip through SDRAM —
 * otherwise repeat reads within a cache line return cached data and
 * miss write-disturb errors that occur after first refill.
 */

#include "bench.h"
#include "patterns.h"

/* ── Bootdata layout (mirror of hw/rom/bootdata.h) ────────────── */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2
#define ACFG_CLASS_MEMORY   1     /* matches penumbra_pkg::ACFG_CLASS_MEMORY */

struct btag_hdr {
    uint32_t type;
    uint32_t size;
};

struct btag_device {
    struct btag_hdr hdr;
    uint32_t cls;
    uint32_t base;
    uint32_t dev_size;
    uint32_t id;
    char     name[16];
};

/* ── Test window policy ───────────────────────────────────────── */
/*
 * STACK_TOP in crt0.S is 0x00100000 (1 MB).  We reserve the entire
 * first 2 MB so there's room for stack growth, .bss, and any
 * unaligned image base the ROM picks (find_memory_region rounds up
 * to a 4 KB page boundary, but we want a comfortable margin).
 */
#define MEMTEST_RESERVED_LO  (2 * 1024 * 1024)

/*
 * Default test window — small enough that all six patterns finish
 * in a few seconds, large enough to span multiple SDRAM rows
 * (which is the unit of address-line aliasing for the W9825:
 * row = bits [22:10] of the linear-byte address).
 *
 * Walking-1 / walking-0 are O(N×32) memory ops, so 8 MB is the
 * natural ceiling before they start dominating run time.
 */
#define MEMTEST_MAX_BYTES   (8 * 1024 * 1024)

/* ── Pretty hex / size printing ──────────────────────────────── */
static void print_hex32(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    bench_putchar('0');
    bench_putchar('x');
    for (int shift = 28; shift >= 0; shift -= 4)
        bench_putchar(digits[(v >> shift) & 0xF]);
}

static void print_size(uint32_t bytes) {
    if (bytes >= (1u << 20)) {
        bench_print_uint(bytes >> 20);
        bench_puts(" MiB");
    } else if (bytes >= (1u << 10)) {
        bench_print_uint(bytes >> 10);
        bench_puts(" KiB");
    } else {
        bench_print_uint(bytes);
        bench_puts(" B");
    }
}

/* ── Bootdata walker ─────────────────────────────────────────── */
static const struct btag_device *find_first_memory_region(uint32_t bootdata) {
    /* The ROM puts the bootdata at BOOTDATA_BASE on page 0; bootdata
     * arg is just a pointer to the same area.  Validate it matches
     * to be safe, then walk. */
    uint32_t p = bootdata ? bootdata : BOOTDATA_BASE;

    /* Skip the bootdata header (12 bytes) — magic/version/total_size. */
    p += 12;

    for (;;) {
        struct btag_hdr *h = (struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        if (h->type == BTAG_DEVICE) {
            struct btag_device *d = (struct btag_device *)p;
            if (d->cls == ACFG_CLASS_MEMORY)
                return d;
        }
        p += h->size;
    }
}

/* ── Diagnostic (referenced by patterns.c) ───────────────────── */
void mt_report_fail(const char *pat, uint32_t addr,
                    uint32_t expected, uint32_t got) {
    bench_puts("\n  FAIL ");
    bench_puts(pat);
    bench_puts(" @ ");
    print_hex32(addr);
    bench_puts(": expected=");
    print_hex32(expected);
    bench_puts(" got=");
    print_hex32(got);
    bench_puts("\n");
}

void bench_main(uint32_t bootdata) {
    bench_init();
    bench_caches_disable();   /* see file header — every access must hit SDRAM */

    bench_puts("\n=== Penumbra memtest ===\n");

    const struct btag_device *ram = find_first_memory_region(bootdata);
    if (!ram) {
        bench_puts("No ACFG_CLASS_MEMORY entry in bootdata — abort.\n");
        return;
    }

    uint32_t lo = ram->base + MEMTEST_RESERVED_LO;
    uint32_t hi = ram->base + ram->dev_size;
    if (hi - lo > MEMTEST_MAX_BYTES)
        hi = lo + MEMTEST_MAX_BYTES;

    /* Align both ends down to 4 bytes for word-stride patterns. */
    lo = (lo + 3) & ~3u;
    hi =  hi      & ~3u;

    bench_puts("RAM:    base=");
    print_hex32(ram->base);
    bench_puts(" size=");
    print_size(ram->dev_size);
    bench_puts("\nWindow: ");
    print_hex32(lo);
    bench_puts("..");
    print_hex32(hi);
    bench_puts(" (");
    print_size(hi - lo);
    bench_puts(")\n\n");

    unsigned failures = 0;
    for (unsigned i = 0; i < pattern_table_len; i++) {
        const struct pattern_entry *p = &pattern_table[i];
        bench_puts("[");
        bench_print_uint(i + 1);
        bench_puts("/");
        bench_print_uint(pattern_table_len);
        bench_puts("] ");
        bench_puts(p->name);
        bench_puts(" ... ");

        bench_timer_start();
        int rc = p->fn(lo, hi);
        uint32_t us = bench_timer_elapsed_us();

        if (rc == 0)
            bench_puts("OK");
        /* On failure pat_*() already printed the FAIL line */
        bench_puts(" (");
        bench_print_uint(us / 1000);
        bench_puts(" ms)\n");

        if (rc) failures++;
    }

    bench_puts("\n=== ");
    bench_print_uint(pattern_table_len - failures);
    bench_puts("/");
    bench_print_uint(pattern_table_len);
    bench_puts(" patterns passed ===\n");
}

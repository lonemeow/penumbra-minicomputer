/*
 * util_test.c — host-side unit test for the boot ROM's size formatter
 *
 * humanize_size computes in explicitly 64-bit types, so the host
 * computes what the ROM does. That holds only while it does: plain
 * `long` is 32 bits on the ROM and 64 here, and arithmetic in it would
 * pass here and overflow there.
 *
 * Build and run: make -C hw/rom test
 */

#include <stdio.h>
#include <string.h>

#include "../util.h"

static int checks, fails;

static void expect(unsigned long long bytes, const char *want) {
    char got[32];
    humanize_size(bytes, got, sizeof got);
    checks++;
    if (strcmp(got, want) != 0) {
        fails++;
        printf("  FAIL: %llu bytes -> \"%s\", want \"%s\"\n", bytes, got, want);
    }
}

int main(void) {
    expect(0, "0B");
    expect(512, "512B");
    expect(1024, "1kB");
    expect(1536, "1.5kB");
    expect(64ull << 10, "64kB");            /* an L2 cache */
    expect(32ull << 20, "32MB");            /* the ULX3S's SDRAM */
    expect(512ull << 20, "512MB");          /* the Nexys Video's DDR3 */
    expect(1ull << 30, "1GB");
    expect(3ull << 29, "1.5GB");
    expect(2047ull << 20, "1.9GB");         /* truncated, not rounded */
    expect(4064ull << 20, "3.9GB");         /* remainder x 10 past 32 bits */
    expect(32ull << 30, "32GB");            /* a partition past 4 GB */
    expect(0xFFFFFFFFull * 512, "1.9TB");   /* the largest MBR partition */

    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails != 0;
}

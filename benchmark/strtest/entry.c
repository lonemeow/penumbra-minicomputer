/*
 * entry.c — correctness test for the shared common/lib/libc string
 * routines (strlen / strcmp / strcpy), exercised in the bare-metal
 * environment they also run in inside real programs.
 *
 * Not a benchmark: it self-checks and prints "=== N/M checks passed ===".
 * Built like memtest (PIE, crt0.S sets up the MMU + UART), linked against
 * libpenstring.a.  Run from the ROM monitor:  boot sd:0,0/STRTEST.ELF
 *
 * Compiled with -fno-builtin so each call reaches the real routine
 * rather than a clang-expanded or constant-folded builtin.
 */

#include "bench.h"

/* Routines under test (declared here so -fno-builtin still type-checks). */
unsigned long strlen(const char *s);
int           strcmp(const char *a, const char *b);
char         *strcpy(char *dst, const char *src);

static uint32_t checks;
static uint32_t fails;

/* Aligned scratch so we can place a string at a chosen +0..+3 offset. */
static char abuf[128] __attribute__((aligned(16)));
static char bbuf[128] __attribute__((aligned(16)));

static void fail_uu(const char *what, uint32_t a, uint32_t b,
                    uint32_t got, uint32_t want)
{
    fails++;
    bench_puts("FAIL ");
    bench_puts(what);
    bench_puts(" arg=");
    bench_print_uint(a);
    bench_puts("/");
    bench_print_uint(b);
    bench_puts(" got=");
    bench_print_int((int)got);
    bench_puts(" want=");
    bench_print_int((int)want);
    bench_puts("\n");
}

/*
 * strlen: every start alignment (0..3) crossed with every length
 * (0..48) — covers the unaligned head, the word loop, and the tail.
 * The fill includes high-bit bytes (>= 0x80) right up against the
 * terminator: that is the case a naive detector false-positives on.
 */
static void test_strlen(void)
{
    for (int off = 0; off < 4; off++) {
        for (int len = 0; len <= 48; len++) {
            char *s = abuf + off;
            for (int i = 0; i < len; i++) {
                unsigned char v = (unsigned char)(i * 13 + 1);
                s[i] = (char)(v ? v : 0xAB);    /* never NUL */
            }
            s[len] = '\0';
            checks++;
            unsigned long got = strlen(s);
            if (got != (unsigned long)len)
                fail_uu("strlen", (uint32_t)off, (uint32_t)len,
                        (uint32_t)got, (uint32_t)len);
        }
    }
}

/* strcmp: equal strings, and a single byte differing at each position,
 * across all alignment pairs.  Checks the sign, not just nonzero. */
static void test_strcmp(void)
{
    for (int oa = 0; oa < 4; oa++) {
        for (int ob = 0; ob < 4; ob++) {
            for (int len = 0; len <= 20; len++) {
                char *a = abuf + oa;
                char *b = bbuf + ob;
                for (int i = 0; i < len; i++) a[i] = b[i] = (char)('A' + (i % 26));
                a[len] = b[len] = '\0';

                /* equal */
                checks++;
                if (strcmp(a, b) != 0)
                    fail_uu("strcmp-eq", (uint32_t)oa, (uint32_t)ob,
                            (uint32_t)strcmp(a, b), 0);

                /* differ at position p: a[p] bumped up -> a > b */
                for (int p = 0; p < len; p++) {
                    char save = a[p];
                    a[p] = save + 1;
                    checks++;
                    if (strcmp(a, b) <= 0)
                        fail_uu("strcmp-gt", (uint32_t)oa, (uint32_t)p, 1, 0);
                    checks++;
                    if (strcmp(b, a) >= 0)
                        fail_uu("strcmp-lt", (uint32_t)oa, (uint32_t)p, 1, 0);
                    a[p] = save;
                }
            }
        }
    }
}

/* strcpy: copy, then verify content (via strcmp), terminator, and that
 * the return value is the original dst. */
static void test_strcpy(void)
{
    for (int oa = 0; oa < 4; oa++) {
        for (int ob = 0; ob < 4; ob++) {
            for (int len = 0; len <= 40; len++) {
                char *src = abuf + oa;
                char *dst = bbuf + ob;
                for (int i = 0; i < len; i++) src[i] = (char)('a' + (i % 26));
                src[len] = '\0';
                dst[len + 1] = (char)0x7e;          /* sentinel past the copy */

                char *ret = strcpy(dst, src);
                checks++;
                if (ret != dst)
                    fail_uu("strcpy-ret", (uint32_t)oa, (uint32_t)ob, 1, 0);
                checks++;
                if (strcmp(dst, src) != 0)
                    fail_uu("strcpy-content", (uint32_t)oa, (uint32_t)len, 1, 0);
                checks++;
                if (dst[len] != '\0')
                    fail_uu("strcpy-term", (uint32_t)oa, (uint32_t)len, 1, 0);
                checks++;
                if (dst[len + 1] != (char)0x7e)      /* must not overrun */
                    fail_uu("strcpy-overrun", (uint32_t)oa, (uint32_t)len, 1, 0);
            }
        }
    }
}

void bench_main(uint32_t bootdata)
{
    (void)bootdata;
    bench_puts("\n=== Penumbra string-routine test ===\n");

    test_strlen();
    test_strcmp();
    test_strcpy();

    bench_puts("\n=== ");
    bench_print_uint(checks - fails);
    bench_puts("/");
    bench_print_uint(checks);
    bench_puts(" checks passed ===\n");
    if (fails)
        bench_puts("*** FAILURES PRESENT ***\n");
}

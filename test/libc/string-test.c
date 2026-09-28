/*
 * Correctness test for the hand-written string routines in
 * netbsd/common/lib/libc/arch/penumbra/string/.
 *
 * The runner assembles each routine with its symbol renamed to pen_<name>
 * and runs this program on the ISS in hosted mode.  It links no libc: a
 * stray call the compiler plants to memcpy or memset would fail to link
 * rather than exercise the wrong code.  Every routine is checked against a plain reference
 * across lengths 0..MAX_SMALL and a few large sizes, every source and
 * destination alignment, and guard bytes on both sides of the destination:
 * a routine that stores a byte outside its range fails here even when the
 * bytes it was meant to write are right.
 */

#include <stddef.h>

void  *pen_memcpy(void *dst, const void *src, size_t len);
void  *pen_memset(void *dst, int c, size_t len);
int    pen_memcmp(const void *a, const void *b, size_t len);
size_t pen_strlen(const char *s);
int    pen_strcmp(const char *a, const char *b);
char  *pen_strcpy(char *dst, const char *src);

#define MAX_SMALL 72
#define MAX_LEN   1100
#define PAD       8                     /* guard bytes either side */
#define BUF       (PAD + 4 + MAX_LEN + 1 + PAD)
#define GUARD     0xA5

static unsigned char src[BUF], dst[BUF];
static int failures;

void hosted_write(int fd, const char *buf, int len);

static void put(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    hosted_write(1, s, n);
}

static void put_int(int v)
{
    char buf[12];
    int i = sizeof(buf);
    unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[--i] = '-';
    hosted_write(1, buf + i, (int)sizeof(buf) - i);
}

static void copy(void *to, const void *from, int len)
{
    unsigned char *d = to;
    const unsigned char *f = from;
    while (len-- > 0)
        *d++ = *f++;
}

static const int large[] = { 127, 128, 129, 255, 256, 257, 1023, 1024, 1027, MAX_LEN };

/* Lengths under test: every small length, then the large ones. */
static int length(int i)
{
    return i <= MAX_SMALL ? i : large[i - MAX_SMALL - 1];
}
#define NLENGTHS (MAX_SMALL + 1 + (int)(sizeof(large) / sizeof(large[0])))

/* A nonzero pattern that includes bytes with the top bit set. */
static unsigned char pat(int i)
{
    return (unsigned char)(i % 251 + 3);
}

static void fail(const char *what, int len, int so, int dof, int detail)
{
    if (failures < 25) {
        put("FAIL ");
        put(what);
        put(": len=");
        put_int(len);
        put(" src+");
        put_int(so);
        put(" dst+");
        put_int(dof);
        put(" (");
        put_int(detail);
        put(")\n");
    }
    failures++;
}

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

static void fill(unsigned char *buf, unsigned char v)
{
    int i;
    for (i = 0; i < BUF; i++)
        buf[i] = v;
}

static void pattern(unsigned char *buf, int base)
{
    int i;
    for (i = 0; i < BUF; i++)
        buf[i] = pat(base + i);
}

/* Bytes of dst outside [lo, hi) must still be GUARD. */
static int guards_intact(int lo, int hi)
{
    int i;
    for (i = 0; i < BUF; i++)
        if ((i < lo || i >= hi) && dst[i] != GUARD)
            return 0;
    return 1;
}

static void test_memcpy(void)
{
    int n, so, dof, i;
    for (n = 0; n < NLENGTHS; n++)
        for (so = 0; so < 4; so++)
            for (dof = 0; dof < 4; dof++) {
                int len = length(n), s = PAD + so, d = PAD + dof;
                pattern(src, 0);
                fill(dst, GUARD);
                if (pen_memcpy(dst + d, src + s, len) != dst + d)
                    fail("memcpy return", len, so, dof, 0);
                for (i = 0; i < len; i++)
                    if (dst[d + i] != src[s + i]) {
                        fail("memcpy data", len, so, dof, i);
                        break;
                    }
                if (!guards_intact(d, d + len))
                    fail("memcpy overrun", len, so, dof, 0);
            }
}

static void test_memset(void)
{
    static const int values[] = { 0, 0x5A, 0xFF, 0x1234 };
    int n, dof, v, i;
    for (n = 0; n < NLENGTHS; n++)
        for (dof = 0; dof < 4; dof++)
            for (v = 0; v < 4; v++) {
                int len = length(n), d = PAD + dof;
                fill(dst, GUARD);
                if (pen_memset(dst + d, values[v], len) != dst + d)
                    fail("memset return", len, 0, dof, v);
                for (i = 0; i < len; i++)
                    if (dst[d + i] != (unsigned char)values[v]) {
                        fail("memset data", len, 0, dof, i);
                        break;
                    }
                if (!guards_intact(d, d + len))
                    fail("memset overrun", len, 0, dof, v);
            }
}

static void test_memcmp(void)
{
    int n, so, dof, pos;
    for (n = 0; n < NLENGTHS; n++)
        for (so = 0; so < 4; so++)
            for (dof = 0; dof < 4; dof++) {
                int len = length(n);
                unsigned char *a = src + PAD + so, *b = dst + PAD + dof;
                pattern(src, 0);
                copy(b, a, len);
                if (pen_memcmp(a, b, len) != 0)
                    fail("memcmp equal", len, so, dof, 0);
                /* A difference at each position, both ways, including a
                 * top-bit byte against a small one (bytes compare
                 * unsigned).  Large lengths sample the positions. */
                for (pos = 0; pos < len; pos += len > MAX_SMALL ? 61 : 1) {
                    unsigned char keep = b[pos];
                    b[pos] = (unsigned char)(a[pos] + 1);
                    if (sign(pen_memcmp(a, b, len)) != sign((int)a[pos] - (int)b[pos]))
                        fail("memcmp +1", len, so, dof, pos);
                    b[pos] = (unsigned char)(a[pos] ^ 0x80);
                    if (sign(pen_memcmp(a, b, len)) != sign((int)a[pos] - (int)b[pos]))
                        fail("memcmp ^80", len, so, dof, pos);
                    b[pos] = keep;
                }
            }
}

/* A string of len nonzero bytes at buf + off, then NUL, then garbage. */
static char *make_string(unsigned char *buf, int off, int len)
{
    int i;
    pattern(buf, 1);
    for (i = 0; i < len; i++)
        if (buf[off + i] == 0)
            buf[off + i] = 1;
    buf[off + len] = 0;
    return (char *)buf + off;
}

static void test_strlen(void)
{
    int n, so;
    for (n = 0; n < NLENGTHS; n++)
        for (so = 0; so < 4; so++) {
            int len = length(n);
            char *s = make_string(src, PAD + so, len);
            if (pen_strlen(s) != (size_t)len)
                fail("strlen", len, so, 0, (int)pen_strlen(s));
        }
}

static void test_strcpy(void)
{
    int n, so, dof, i;
    for (n = 0; n < NLENGTHS; n++)
        for (so = 0; so < 4; so++)
            for (dof = 0; dof < 4; dof++) {
                int len = length(n), d = PAD + dof;
                char *s = make_string(src, PAD + so, len);
                fill(dst, GUARD);
                if (pen_strcpy((char *)dst + d, s) != (char *)dst + d)
                    fail("strcpy return", len, so, dof, 0);
                for (i = 0; i <= len; i++)
                    if (dst[d + i] != (unsigned char)s[i]) {
                        fail("strcpy data", len, so, dof, i);
                        break;
                    }
                if (!guards_intact(d, d + len + 1))
                    fail("strcpy overrun", len, so, dof, 0);
            }
}

static void test_strcmp(void)
{
    int n, so, dof, pos;
    for (n = 0; n < NLENGTHS; n++)
        for (so = 0; so < 4; so++)
            for (dof = 0; dof < 4; dof++) {
                int len = length(n);
                char *a = make_string(src, PAD + so, len);
                char *b = (char *)dst + PAD + dof;
                copy(b, a, len + 1);
                if (pen_strcmp(a, b) != 0)
                    fail("strcmp equal", len, so, dof, 0);
                for (pos = 0; pos <= len; pos += len > MAX_SMALL ? 61 : 1) {
                    unsigned char keep = (unsigned char)b[pos], next = (unsigned char)b[pos + 1];
                    int expect;
                    /* A different byte at pos; at pos == len that makes b
                     * the longer string. */
                    b[pos] = (char)((unsigned char)a[pos] ^ 0x81);
                    if (pos == len)
                        b[pos + 1] = 0;
                    expect = sign((int)(unsigned char)a[pos] - (int)(unsigned char)b[pos]);
                    if (sign(pen_strcmp(a, b)) != expect)
                        fail("strcmp differ", len, so, dof, pos);
                    /* b truncated at pos: a prefix of a, so a is greater. */
                    if (pos < len) {
                        b[pos] = 0;
                        if (sign(pen_strcmp(a, b)) != 1)
                            fail("strcmp prefix", len, so, dof, pos);
                    }
                    b[pos] = (char)keep;
                    b[pos + 1] = (char)next;
                }
            }
}

int main(void)
{
    test_memcpy();
    test_memset();
    test_memcmp();
    test_strlen();
    test_strcpy();
    test_strcmp();
    if (failures) {
        put("string-test: ");
        put_int(failures);
        put(" failures\n");
    } else {
        put("string-test: all passed\n");
    }
    return failures != 0;
}

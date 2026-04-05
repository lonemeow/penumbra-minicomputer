#include "libc.h"

unsigned long strlen(const char *s) {
    unsigned long i = 0;
    while (*s++ != '\0') {
        i++;
    }
    return i;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned long n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (unsigned long)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

char *strncat(char *dest, const char *src, unsigned long n) {
    char *ptr = dest;

    while (*ptr != '\0') {
        ptr++;
    }

    while (n > 0 && *src != '\0') {
        *ptr++ = *src++;
        n--;
    }

    *ptr = '\0';
   return dest;
}

int isprint(int c) {
    return c >= 0x20 && c <= 0x7E;
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

static const char HEX_CHARS[] = "0123456789abcdef";

char *utoa(unsigned int val, char *buf, int base) {
    buf[11] = '\0';
    char *p = buf + 10;
    do {
        *p-- = HEX_CHARS[val % base];
        val /= base;
    } while (val > 0);
    return p + 1;
}

// Internal helper that calculates both quotient and remainder
static inline void __divmodsi4(unsigned int n, unsigned int d, 
                               unsigned int *qp, unsigned int *rp) {
    unsigned int q = 0;
    unsigned int r = 0;

    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (1U << i);
        }
    }
    if (qp) *qp = q;
    if (rp) *rp = r;
}

// 32-bit Unsigned Multiply (shift-and-add)
unsigned int __mulsi3(unsigned int a, unsigned int b) {
    unsigned int result = 0;
    while (a) {
        if (a & 1)
            result += b;
        a >>= 1;
        b <<= 1;
    }
    return result;
}

// 32-bit Unsigned Division
unsigned int __udivsi3(unsigned int n, unsigned int d) {
    unsigned int q;
    if (d == 0) return 0; // Handle division by zero
    __divmodsi4(n, d, &q, 0);
    return q;
}

// 32-bit Unsigned Modulo
unsigned int __umodsi3(unsigned int n, unsigned int d) {
    unsigned int r;
    if (d == 0) return 0;
    __divmodsi4(n, d, 0, &r);
    return r;
}

// 32-bit Signed Division
int __divsi3(int n, int d) {
    if (d == 0) return 0;
    int neg = 0;
    unsigned int un = (unsigned int)n;
    unsigned int ud = (unsigned int)d;
    if (n < 0) { neg = !neg; un = (unsigned int)(-n); }
    if (d < 0) { neg = !neg; ud = (unsigned int)(-d); }
    unsigned int q;
    __divmodsi4(un, ud, &q, 0);
    return neg ? -(int)q : (int)q;
}

// 32-bit Signed Modulo
int __modsi3(int n, int d) {
    if (d == 0) return 0;
    unsigned int un = (unsigned int)(n < 0 ? -n : n);
    unsigned int ud = (unsigned int)(d < 0 ? -d : d);
    unsigned int r;
    __divmodsi4(un, ud, 0, &r);
    return n < 0 ? -(int)r : (int)r;
}

/* ── String-to-integer ─────────────────────────────────────────────────── */

unsigned long strtoul(const char *s, char **endp, int base) {
    unsigned long val = 0;

    while (*s == ' ') s++;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        if (base == 0) base = 16;
        if (base == 16) s += 2;
    }

    for (;;) {
        int digit;
        char c = *s;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (c >= 'a' && c <= 'f')  digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')  digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        s++;
    }

    if (endp) *endp = s;
    return val;
}

/* ── Formatted output ──────────────────────────────────────────────────── */

/* Helper: safely emit one character to the buffer. */
static int emit(char *buf, unsigned long size, unsigned long pos, char c) {
    if (pos < size - 1)
        buf[pos] = c;
    return 1;
}

/* Helper: emit a string to the buffer. */
static int emit_str(char *buf, unsigned long size, unsigned long pos,
                    const char *s) {
    int n = 0;
    while (*s)
        n += emit(buf, size, pos + n, *s++);
    return n;
}

static int emit_str_padded(char *buf, unsigned long size, unsigned long pos,
                           const char *s, int padwidth, char padchar) {
    int n = 0;
    if (padwidth > 0) {
        int len = strlen(s);
        while (n < (padwidth - len)) {
            n += emit(buf, size, pos + n, padchar);
        }
    }

    return n + emit_str(buf, size, pos + n, s);
}

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap) {
    int pos = 0;
    char scratch[12];

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            char padchar = ' ';
            int padwidth = 0;
            if (isdigit(*fmt)) {
                if (*fmt == '0') {
                    padchar = '0';
                    fmt++;
                }
                padwidth = strtoul(fmt, &fmt, 10);
            }
            switch (*fmt) {
            case '%':
                pos += emit(buf, size, pos, '%');
                break;
            case 'u': {
                unsigned int val = va_arg(ap, unsigned int);
                char *p = utoa(val, scratch, 10);
                pos += emit_str_padded(buf, size, pos, p, padwidth, padchar);
                break;
            }
            case 'd': {
                int val = va_arg(ap, int);
                if (val < 0) {
                    pos += emit(buf, size, pos, '-');
                    val = -val;
                    padwidth -= 1;
                }
                char *p = utoa(val, scratch, 10);
                pos += emit_str_padded(buf, size, pos, p, padwidth, padchar);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(ap, unsigned int);
                char *p = utoa(val, scratch, 16);
                pos += emit_str_padded(buf, size, pos, p, padwidth, padchar);
                break;
            }
            case 's': {
                char *p = va_arg(ap, char *);
                pos += emit_str_padded(buf, size, pos, p, padwidth, padchar);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                pos += emit(buf, size, pos, c);
                break;
            }
            }
        } else {
            emit(buf, size, pos++, *fmt);
        }
        fmt++;
    }
    buf[pos < size ? pos : size-1] = '\0';
    return pos;
}

int snprintf(char *buf, unsigned long size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

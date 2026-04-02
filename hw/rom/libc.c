#include "libc.h"

unsigned long strlen(const char *s) {
    unsigned long i = 0;
    while (*s++ != '\0') {
        i++;
    }
    return i;
}

int isprint(int c) {
    return c >= 0x20 && c <= 0x7E;
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

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap) {
    int pos = 0;
    char scratch[12];

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
            case '%':
                pos += emit(buf, size, pos, '%');
                break;
            case 'u': {
                unsigned int val = va_arg(ap, unsigned int);
                char *p = utoa(val, scratch, 10);
                pos += emit_str(buf, size, pos, p);
                break;
            }
            case 'd': {
                int val = va_arg(ap, int);
                if (val < 0) {
                    pos += emit(buf, size, pos, '-');
                    val = -val;
                }
                char *p = utoa(val, scratch, 10);
                pos += emit_str(buf, size, pos, p);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(ap, unsigned int);
                char *p = utoa(val, scratch, 16);
                pos += emit_str(buf, size, pos, p);
                break;
            }
            case 's': {
                char *p = va_arg(ap, char *);
                pos += emit_str(buf, size, pos, p);
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

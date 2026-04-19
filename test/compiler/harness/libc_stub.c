/*-
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)printf.c	8.1 (Berkeley) 6/11/93
 */

/*
 * Copyright (c) 2026, Penumbra Minicomputer Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 */

// test/compiler/harness/libc_stub.c
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void exit(int status) {
    __asm__ volatile (
        "mov r1, %0\n"
        "lli r11, 1\n"
        "syscall"
        : : "r"(status) : "r1", "r11"
    );
    while(1);
}

int write(int fd, const void *buf, size_t count) {
    int ret;
    __asm__ volatile (
        "mov r1, %1\n"
        "mov r2, %2\n"
        "mov r3, %3\n"
        "lli r11, 4\n"
        "syscall\n"
        "mov %0, r1"
        : "=r"(ret)
        : "r"(fd), "r"(buf), "r"(count)
        : "r1", "r2", "r3", "r11"
    );
    return ret;
}

int putchar(int c) {
    unsigned char ch = (unsigned char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    write(1, s, len);
    putchar('\n');
    return 0;
}

/* ── Formatted output (adapted from NetBSD libsa) ──────────────────────── */

static void kprintn(int (*put)(int), uint64_t ul, int base, int width, int zeropad) {
    char buf[64];
    char *p = buf;
    static const char hexdigits[] = "0123456789abcdef";

    do {
        *p++ = hexdigits[ul % base];
    } while (ul /= base);

    int len = p - buf;
    if (width > 0) {
        while (len < width) {
            put(zeropad ? '0' : ' ');
            width--;
        }
    }

    do {
        put(*--p);
    } while (p > buf);
}

static void print_double(int (*put)(int), double d, int width, int precision) {
    if (d < 0) {
        put('-');
        d = -d;
    }
    
    uint64_t ipart = (uint64_t)d;
    kprintn(put, ipart, 10, 0, 0);
    put('.');
    
    if (precision < 0) precision = 6;
    double fpart = d - (double)ipart;
    for (int i = 0; i < precision; i++) {
        fpart *= 10.0;
        int digit = (int)fpart;
        put(digit + '0');
        fpart -= (double)digit;
    }
}

static void kdoprnt(int (*put)(int), const char *fmt, va_list ap) {
    char *p;
    int ch;
    uint64_t ul;
    int lflag, width, zeropad, precision;

    for (;;) {
        while ((ch = *fmt++) != '%') {
            if (ch == '\0') return;
            put(ch);
        }
        lflag = 0; width = 0; zeropad = 0; precision = -1;
    reswitch:
        switch (ch = *fmt++) {
        case '0':
            if (width == 0) {
                zeropad = 1;
                goto reswitch;
            }
            /* FALLTHROUGH */
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            width = width * 10 + ch - '0';
            goto reswitch;
        case '.':
            precision = 0;
            while ((ch = *fmt) >= '0' && ch <= '9') {
                precision = precision * 10 + ch - '0';
                fmt++;
            }
            goto reswitch;
        case 'l':
            lflag++;
            goto reswitch;
        case 'c':
            put(va_arg(ap, int));
            break;
        case 's':
            p = va_arg(ap, char *);
            if (!p) p = "(null)";
            while (*p) put(*p++);
            break;
        case 'd':
            if (lflag >= 2) ul = va_arg(ap, long long);
            else if (lflag == 1) ul = va_arg(ap, long);
            else ul = va_arg(ap, int);
            
            if ((int64_t)ul < 0) {
                put('-');
                ul = -(int64_t)ul;
            }
            kprintn(put, ul, 10, width, zeropad);
            break;
        case 'u':
            if (lflag >= 2) ul = va_arg(ap, unsigned long long);
            else if (lflag == 1) ul = va_arg(ap, unsigned long);
            else ul = va_arg(ap, unsigned int);
            kprintn(put, ul, 10, width, zeropad);
            break;
        case 'x':
            if (lflag >= 2) ul = va_arg(ap, unsigned long long);
            else if (lflag == 1) ul = va_arg(ap, unsigned long);
            else ul = va_arg(ap, unsigned int);
            kprintn(put, ul, 16, width, zeropad);
            break;
        case 'p':
            put('0'); put('x');
            ul = (uintptr_t)va_arg(ap, void *);
            kprintn(put, ul, 16, 8, 1);
            break;
        case 'f':
            print_double(put, va_arg(ap, double), width, precision);
            break;
        case '%':
            put('%');
            break;
        }
    }
}

static char *sbuf, *ebuf;
static int sputchar(int c) {
    if (sbuf < ebuf) *sbuf++ = c;
    return c;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    sbuf = buf;
    ebuf = buf + size - 1;
    kdoprnt(sputchar, fmt, ap);
    if (buf != NULL && size > 0) *sbuf = '\0';
    return sbuf - buf;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    sbuf = buf;
    ebuf = (char *)(uintptr_t)-1; // No limit
    kdoprnt(sputchar, fmt, ap);
    *sbuf = '\0';
    return sbuf - buf;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    kdoprnt(putchar, fmt, ap);
    return 0;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kdoprnt(putchar, fmt, ap);
    va_end(ap);
    return 0;
}

/* ── Memory management stubs ───────────────────────────────────────────── */

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 1 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

/* ── Compiler runtime (math) ───────────────────────────────────────────── */

static inline void __divmodsi4(uint32_t n, uint32_t d,
                               uint32_t *qp, uint32_t *rp) {
    uint32_t q = 0;
    uint32_t r = 0;
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

uint32_t __mulsi3(uint32_t a, uint32_t b) {
    uint32_t result = 0;
    while (a) {
        if (a & 1) result += b;
        a >>= 1;
        b <<= 1;
    }
    return result;
}

uint32_t __udivsi3(uint32_t n, uint32_t d) {
    uint32_t q;
    if (d == 0) return 0;
    __divmodsi4(n, d, &q, 0);
    return q;
}

uint32_t __umodsi3(uint32_t n, uint32_t d) {
    uint32_t r;
    if (d == 0) return 0;
    __divmodsi4(n, d, 0, &r);
    return r;
}

int __divsi3(int n, int d) {
    if (d == 0) return 0;
    int neg = 0;
    uint32_t un = (n < 0) ? (uint32_t)-n : (uint32_t)n;
    uint32_t ud = (d < 0) ? (uint32_t)-d : (uint32_t)d;
    if (n < 0) neg = !neg;
    if (d < 0) neg = !neg;
    uint32_t q;
    __divmodsi4(un, ud, &q, 0);
    return neg ? -(int)q : (int)q;
}

int __modsi3(int n, int d) {
    if (d == 0) return 0;
    uint32_t un = (n < 0) ? (uint32_t)-n : (uint32_t)n;
    uint32_t ud = (d < 0) ? (uint32_t)-d : (uint32_t)d;
    uint32_t r;
    __divmodsi4(un, ud, 0, &r);
    return n < 0 ? -(int)r : (int)r;
}

int abs(int j) {
    return j < 0 ? -j : j;
}

int atoi(const char *nptr) {
    int res = 0;
    int sign = 1;
    while (*nptr == ' ' || (*nptr >= 9 && *nptr <= 13)) nptr++;
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    } else if (*nptr == '+') {
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        res = res * 10 + (*nptr - '0');
        nptr++;
    }
    return res * sign;
}

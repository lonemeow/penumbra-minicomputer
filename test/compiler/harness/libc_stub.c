// test/compiler/harness/libc_stub.c
#include <stdarg.h>
#include <stddef.h>

typedef unsigned int uint32_t;

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

static void print_int(int n) {
    char buf[12];
    int i = 0;
    unsigned int u;
    if (n < 0) {
        putchar('-');
        u = -n;
    } else {
        u = n;
    }
    if (u == 0) {
        putchar('0');
        return;
    }
    while (u > 0) {
        buf[i++] = (u % 10) + '0';
        u /= 10;
    }
    while (i > 0) {
        putchar(buf[--i]);
    }
}

static void print_hex(unsigned int u) {
    char buf[8];
    int i = 0;
    if (u == 0) {
        putchar('0');
        return;
    }
    while (u > 0) {
        int d = u % 16;
        buf[i++] = (d < 10) ? (d + '0') : (d - 10 + 'a');
        u /= 16;
    }
    while (i > 0) {
        putchar(buf[--i]);
    }
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    while (*format) {
        if (*format == '%') {
            format++;
            switch (*format) {
                case 'd': print_int(va_arg(ap, int)); break;
                case 'x': print_hex(va_arg(ap, unsigned int)); break;
                case 's': {
                    char *s = va_arg(ap, char *);
                    while (*s) putchar(*s++);
                    break;
                }
                case 'c': putchar(va_arg(ap, int)); break;
                case '%': putchar('%'); break;
            }
        } else {
            putchar(*format);
        }
        format++;
    }
    va_end(ap);
    return 0;
}

// Memory management stubs for tests that might need them
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

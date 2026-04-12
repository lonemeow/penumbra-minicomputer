/*
 * shim.c — System call shims for Dhrystone bare-metal build
 *
 * Provides printf, scanf, malloc, and time() using the benchmark
 * harness UART and timer.  These are called outside the measurement
 * loop (except strcpy/strcmp which are in string.c).
 *
 * printf: Minimal formatter — %d, %s, %c, %6.1f (enough for
 *         Dhrystone's result output).
 * scanf:  Returns a fixed iteration count (no interactive input).
 * malloc: Bump allocator (Dhrystone calls it exactly twice).
 * time:   Returns elapsed seconds from bench_timer.
 */

#include "bench.h"

/* ── malloc: static bump allocator ─────────────────────────────── *
 * Returns char* to match the K&R declaration in dhry_1.c:
 *   extern char *malloc();
 */

static char heap[4096];
static unsigned long heap_offset;

char *malloc(unsigned long size) {
    /* Align to 4 bytes */
    heap_offset = (heap_offset + 3) & ~3UL;
    if (heap_offset + size > sizeof(heap))
        return (char *)0;
    char *p = &heap[heap_offset];
    heap_offset += size;
    return p;
}

/* ── time: return elapsed seconds ──────────────────────────────── */

long time(long *p) {
    uint32_t us = bench_timer_elapsed_us();
    long sec = (long)(us / 1000000);
    if (p) *p = sec;
    return sec;
}

/* ── printf: minimal formatter ─────────────────────────────────── */

#include <stdarg.h>

static void print_decimal(int val) {
    if (val < 0) {
        bench_putchar('-');
        bench_print_uint((uint32_t)(-val));
    } else {
        bench_print_uint((uint32_t)val);
    }
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                bench_putchar('\r');
            bench_putchar(*fmt++);
            count++;
            continue;
        }
        fmt++; /* skip '%' */

        /* Skip width/precision (e.g. "6.1" in "%6.1f") */
        while ((*fmt >= '0' && *fmt <= '9') || *fmt == '.' || *fmt == '-')
            fmt++;

        switch (*fmt) {
        case 'd': {
            int val = va_arg(ap, int);
            print_decimal(val);
            count++;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            bench_puts(s);
            count++;
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            bench_putchar(c);
            count++;
            break;
        }
        case 'f': {
            /*
             * Dhrystone prints "%6.1f" for Microseconds and
             * Dhrystones_Per_Second.  Soft-float is expensive
             * but this only runs once after the measurement.
             * We consume the double arg and print an integer
             * approximation — good enough for a benchmark report.
             */
            double val = va_arg(ap, double);
            if (val < 0.0) {
                bench_putchar('-');
                val = -val;
            }
            uint32_t whole = (uint32_t)val;
            uint32_t frac = (uint32_t)((val - (double)whole) * 10.0);
            bench_print_uint(whole);
            bench_putchar('.');
            bench_print_uint(frac);
            count++;
            break;
        }
        case '%':
            bench_putchar('%');
            count++;
            break;
        case '\0':
            goto done;
        default:
            bench_putchar('%');
            bench_putchar(*fmt);
            count++;
            break;
        }
        fmt++;
    }
done:
    va_end(ap);
    return count;
}

/* ── scanf: return fixed iteration count ───────────────────────── */

#ifndef DHRYSTONE_ITERATIONS
#define DHRYSTONE_ITERATIONS 100
#endif

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Dhrystone calls: scanf("%d", &n) */
    int *p = va_arg(ap, int *);
    *p = DHRYSTONE_ITERATIONS;
    va_end(ap);
    return 1;
}

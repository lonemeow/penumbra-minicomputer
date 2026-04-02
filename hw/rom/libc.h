#ifndef LIBC_H
#define LIBC_H

#include <stdarg.h>

unsigned long strlen(const char *s);
int isprint(int c);

/* Convert unsigned value to string in buf (must hold ≥12 chars for decimal,
 * ≥9 for hex).  base is 10 or 16.  Returns pointer into buf where the
 * number starts (digits are written right-to-left). */
char *utoa(unsigned int val, char *buf, int base);

/* Formatted output to buffer.  Supports: %d %u %x %s %c %%.
 * Returns number of characters that would have been written
 * (excluding NUL), even if truncated. */
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);

#endif

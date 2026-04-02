#ifndef LIBC_H
#define LIBC_H

#include <stdarg.h>

unsigned long strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);
int isprint(int c);
int isdigit(int c);

/* Convert unsigned value to string in buf (must hold ≥12 chars for decimal,
 * ≥9 for hex).  base is 10 or 16.  Returns pointer into buf where the
 * number starts (digits are written right-to-left). */
char *utoa(unsigned int val, char *buf, int base);

/* Parse an unsigned integer from a string.  Supports 0x prefix for hex,
 * otherwise decimal.  If endp is non-NULL, stores pointer to first
 * unconsumed character.  Returns 0 on empty/invalid input. */
unsigned long strtoul(const char *s, char **endp, int base);

/* Formatted output to buffer.  Supports: %d %u %x %s %c %%.
 * Returns number of characters that would have been written
 * (excluding NUL), even if truncated. */
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);

#endif

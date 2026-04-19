// test/compiler/harness/stdio.h
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
int write(int fd, const void *buf, size_t count);

#endif

// test/compiler/harness/stdio.h
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *format, ...);
int putchar(int c);
int puts(const char *s);
int write(int fd, const void *buf, size_t count);

#endif

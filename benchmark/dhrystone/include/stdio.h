/*
 * stdio.h — Shim for Dhrystone bare-metal build
 *
 * Provides declarations that dhry.h and dhry_1.c expect from <stdio.h>.
 * Implementations are in shim.c using the benchmark harness UART output.
 */

#ifndef _SHIM_STDIO_H
#define _SHIM_STDIO_H

/* String functions (used inside the measurement loop) */
extern char *strcpy(char *dst, const char *src);
extern int   strcmp(const char *a, const char *b);

/* I/O (used outside the measurement loop for reporting) */
int printf(const char *fmt, ...);
int scanf(const char *fmt, ...);

/* malloc: dhry_1.c provides its own K&R declaration
 * (extern char *malloc()), so we don't declare it here.
 * Implementation is in shim.c. */

#endif

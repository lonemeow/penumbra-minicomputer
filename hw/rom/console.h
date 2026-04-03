/*
 * console.h — Console I/O for Penumbra boot ROM
 *
 * Line-editing input, formatted output over UART.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

void console_putc(char c);
void console_puts(const char *s);

/*
 * Read a line with editing into buffer (max chars incl. NUL).
 * Supports backspace, DEL, Ctrl-U (kill line), Enter.
 * Returns the length of the entered string (not counting NUL).
 */
int console_gets(char *buffer, int max);

/* printf-style formatted output to console. */
void console_printf(const char *fmt, ...);

#endif /* CONSOLE_H */

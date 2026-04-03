/*
 * console.c — Console I/O for Penumbra boot ROM
 *
 * Line-editing input and formatted output over UART.
 */

#include "console.h"
#include "uart.h"
#include "libc.h"
#include <stdarg.h>

void console_putc(char c) {
    uart_write(c);
}

void console_puts(const char *s) {
    while (*s) {
        uart_write(*s++);
    }
}

/*
 * console_gets — read a line with editing into buffer (max chars incl. NUL).
 *
 * Supports:
 *   - Printable characters (0x20-0x7E): insert at cursor
 *   - Backspace (0x08) or DEL (0x7F): delete character before cursor
 *   - Ctrl-U (0x15): kill entire line
 *   - Enter (\r or \n): accept line
 *
 * Echoes characters as typed. Backspace sends "\b \b" (back, space, back)
 * to erase the character on the terminal.
 *
 * Returns the length of the entered string (not counting NUL).
 */
int console_gets(char *buffer, int max) {
    int i = 0;

    for (;;) {
        int c = uart_read();

        if (c == '\r' || c == '\n') {
            console_puts("\r\n");
            break;
        }

        switch (c) {
            case 0x08:
            case 0x7F:
                if (i > 0) {
                    i--;
                    console_puts("\b \b");
                }
                break;
            case 0x15:
                for (; i>0; i--) {
                    console_puts("\b \b");
                }
                break;

            default:
                if (isprint(c) && i < (max - 1)) {
                    buffer[i++] = (char)c;
                    console_putc(c);
                }
        }
    }

    buffer[i] = '\0';
    return i;
}

void console_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_puts(buf);
}

/* console.c — polled UART output. */

#include "rt_internal.h"

#define UART_BASE  0xFF000000
#define UART_THR   (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_LSR   (*(volatile uint32_t *)(UART_BASE + 0x14))
#define LSR_THRE   0x20

void rt_putchar(int c)
{
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (uint32_t)c & 0xFF;
}

void rt_puts(const char *s)
{
    while (*s)
        rt_putchar(*s++);
}

void rt_print_uint(uint32_t v)
{
    char buf[10];
    int n = 0;

    if (v == 0) {
        rt_putchar('0');
        return;
    }
    while (v) {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n--)
        rt_putchar(buf[n]);
}

void rt_panic(const char *msg)
{
    rt_puts("\r\n*** demo halted: ");
    rt_puts(msg);
    rt_puts("\r\n");
    for (;;)
        rt_break();
}

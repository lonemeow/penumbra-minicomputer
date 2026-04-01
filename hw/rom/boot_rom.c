/*
 * boot_rom.c — Penumbra/1 Boot ROM (C version)
 *
 * Minimal ROM monitor.  Compiled with:
 *   clang --target=penumbra-unknown-none -S -O0 boot_rom.c
 *
 * UART is memory-mapped at 0xFF00_0000, NS16450-compatible.
 * All I/O is polling-based (no interrupts).
 */

#define UART_BASE  ((volatile unsigned int *)0xFF000000)
#define UART_DATA  (UART_BASE[0])       /* RBR / THR at offset 0x00 */
#define UART_LSR   (UART_BASE[5])       /* LSR at offset 0x14 (word-strided) */
#define LSR_DR     0x01                 /* Data Ready */
#define LSR_THRE   0x20                 /* TX Holding Register Empty */

static void putchar(int c) {
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_DATA = (unsigned int)c;
}

static void puts(const char *s) {
    while (*s)
        putchar(*s++);
}

void main(void) {
    puts("Penumbra/1\r\n");

    /* Spin — placeholder for command loop */
    for (;;)
        ;
}

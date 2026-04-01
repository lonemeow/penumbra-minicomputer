#include "uart.h"

#define UART_BASE  ((volatile unsigned int *)0xFF000000)
#define UART_DATA  (UART_BASE[0])       /* RBR / THR at offset 0x00 */
#define UART_LSR   (UART_BASE[5])       /* LSR at offset 0x14 (word-strided) */
#define LSR_DR     0x01                 /* Data Ready */
#define LSR_THRE   0x20                 /* TX Holding Register Empty */

void uart_write(char c) {
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_DATA = (unsigned int)c;
}

int uart_read(void) {
    while (!(UART_LSR & LSR_DR))
        ;
    return (int)UART_DATA;
}

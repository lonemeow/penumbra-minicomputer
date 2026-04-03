/*
 * cons.c — Console I/O for libsa standalone bootloader
 *
 * Provides putchar() and getchar() required by libsa's printf/kgets.
 * Talks directly to the NS16450 UART at 0xFF000000 (word-strided).
 */

#include <lib/libsa/stand.h>

#define UART_BASE	((volatile uint32_t *)0xFF000000)
#define UART_DATA	(UART_BASE[0])		/* RBR/THR at offset 0x00 */
#define UART_LSR	(UART_BASE[5])		/* LSR at offset 0x14 */
#define LSR_DR		0x01			/* Data Ready */
#define LSR_THRE	0x20			/* TX Holding Register Empty */

void
putchar(int c)
{
	if (c == '\n')
		putchar('\r');
	while (!(UART_LSR & LSR_THRE))
		;
	UART_DATA = (uint32_t)c;
}

int
getchar(void)
{
	while (!(UART_LSR & LSR_DR))
		;
	return (int)(UART_DATA & 0xFF);
}

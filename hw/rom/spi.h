/*
 * spi.h — Low-level SPI master driver for Penumbra boot ROM
 *
 * Stateless inline functions — every call takes the SPI controller's
 * MMIO base address. No globals (ROM has no writable data section).
 * The base address comes from the boot data device list.
 */

#ifndef SPI_H
#define SPI_H

#include "penumbra.h"

/* SPI register offsets (word-strided, index into uint32_t*) */
#define SPI_DATA     0   /* R/W: TX/RX byte */
#define SPI_STATUS   1   /* R:   bit 0 = BUSY, bit 1 = DONE */
#define SPI_CONTROL  2   /* R/W: bit 0 = CS0, bit 1 = CS1 */
#define SPI_CLKDIV   3   /* R/W: clock divider */

#define SPI_STATUS_BUSY  0x01

/* Full-duplex byte exchange: send tx, return rx. Blocks until done. */
static inline unsigned char spi_transfer(uint32_t base, unsigned char tx) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    regs[SPI_DATA] = tx;
    while (regs[SPI_STATUS] & SPI_STATUS_BUSY)
        ;
    return (unsigned char)regs[SPI_DATA];
}

/* Assert/deassert CS0 (active low). 0 = assert, 1 = deassert. */
static inline void spi_cs0(uint32_t base, int state) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    unsigned int ctl = regs[SPI_CONTROL];
    if (state)
        ctl |= 0x01u;
    else
        ctl &= ~0x01u;
    regs[SPI_CONTROL] = ctl;
}

/* Set clock divider. SPI_CLK = CLK / (2 * (div + 1)). */
static inline void spi_set_clkdiv(uint32_t base, unsigned int div) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    regs[SPI_CLKDIV] = div;
}

#endif /* SPI_H */

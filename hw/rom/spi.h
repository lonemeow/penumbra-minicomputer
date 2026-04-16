/*
 * spi.h — Low-level SPI master driver for Penumbra boot ROM
 *
 * Stateless inline functions — every call takes the SPI controller's
 * MMIO base address. No globals (ROM has no writable data section).
 * The base address comes from the boot data device list.
 *
 * Register layout: SPI v2 (see doc/boot/spi-controller.md).
 * Boot ROM uses single-byte polled mode only (FIFO_EN=0).
 */

#ifndef SPI_H
#define SPI_H

#include "penumbra.h"

/* SPI v2 register offsets (word-strided, index into uint32_t*) */
#define SPI_CAP        0   /* R:   version [7:0], FIFO depth [23:8] */
#define SPI_STATUS     1   /* R:   bit 0 = BUSY, bit 1 = DONE */
#define SPI_CONTROL    2   /* R/W: CS, mode, speed, FIFO_EN */
#define SPI_DATA       3   /* R/W: TX/RX byte */
#define SPI_XFER_COUNT 4   /* R/W: transfer count + START */
#define SPI_IRQ_STATUS 5   /* R/W1C: interrupt flags */
#define SPI_IRQ_ENABLE 6   /* R/W: interrupt mask */

#define SPI_STATUS_BUSY  0x01

/* CONTROL register bits */
#define SPI_CTL_CS0      0x01
#define SPI_CTL_CS1      0x02
#define SPI_CTL_CPOL     0x10
#define SPI_CTL_CPHA     0x20
#define SPI_CTL_FAST     0x40
#define SPI_CTL_FIFO_EN  0x80

/* Full-duplex byte exchange: send tx, return rx. Blocks until done.
 * Uses single-byte polled mode (FIFO_EN must be 0). */
static inline unsigned char spi_transfer(uint32_t base, unsigned char tx) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    regs[SPI_DATA] = tx;
    while (regs[SPI_STATUS] & SPI_STATUS_BUSY)
        ;
    return (unsigned char)regs[SPI_DATA];
}

/* Assert/deassert CS0 (active low). 0 = assert, 1 = deassert.
 * Preserves other CONTROL bits. */
static inline void spi_cs0(uint32_t base, int state) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    unsigned int ctl = regs[SPI_CONTROL];
    if (state)
        ctl |= SPI_CTL_CS0;
    else
        ctl &= ~SPI_CTL_CS0;
    regs[SPI_CONTROL] = ctl;
}

/* Switch to fast SPI clock (for post-init SD transfers). */
static inline void spi_fast(uint32_t base) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    regs[SPI_CONTROL] = regs[SPI_CONTROL] | SPI_CTL_FAST;
}

/* Switch to slow SPI clock (for SD card init, ≤400 kHz). */
static inline void spi_slow(uint32_t base) {
    volatile unsigned int *regs = (volatile unsigned int *)base;
    regs[SPI_CONTROL] = regs[SPI_CONTROL] & ~SPI_CTL_FAST;
}

#endif /* SPI_H */

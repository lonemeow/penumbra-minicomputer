# SPI Controller — Programmer's Reference

A SPI master peripheral for the Penumbra bus, primarily used for SD card access (SPI mode).

## Register Interface

Seven word-strided registers. Base address assigned by autoconfig.

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| `0x00` | CAP | R | Capability: version, FIFO depth |
| `0x04` | STATUS | R | Transfer state, FIFO levels and flags |
| `0x08` | CONTROL | R/W | CS pins, SPI mode, clock speed, FIFO enable |
| `0x0C` | DATA | R/W | TX/RX data byte |
| `0x10` | XFER_COUNT | R/W | Autonomous transfer: byte count + START |
| `0x14` | IRQ_STATUS | R/W | Interrupt status (mixed latched + live) |
| `0x18` | IRQ_ENABLE | R/W | Per-source interrupt mask |

### Register Details

#### CAP (0x00)
- Bits [7:0]: Version (1 = SPI v2)
- Bits [23:8]: FIFO depth (entry count)

#### STATUS (0x04)
- Bit [0]: SPI_BUSY
- Bit [1]: SPI_DONE (single-byte complete)
- Bits [15:4]: TX_LEVEL
- Bits [27:16]: RX_LEVEL
- Bit [28]: TX_EMPTY, [29]: TX_FULL
- Bit [30]: RX_EMPTY, [31]: RX_FULL

#### CONTROL (0x08)
- Bit [0]: CS0 (0=assert, 1=deassert)
- Bit [1]: CS1
- Bit [4]: CPOL, Bit [5]: CPHA
- Bit [6]: FAST (0=slow ≤400kHz, 1=fast)
- Bit [7]: FIFO_EN (enable FIFO mode)
- Bit [14]: FLUSH_TX, Bit [15]: FLUSH_RX

#### DATA (0x0C)
In single-byte mode (FIFO_EN=0), writing starts a transfer. In FIFO mode, writing pushes to TX FIFO.

#### XFER_COUNT (0x10)
- Bits [15:0]: COUNT
- Bit [16]: START

## Driver Flow

### Initialization (Polled)
1. Set `CONTROL = 0x03` (CS deasserted, slow clock).
2. Shift 80+ clock cycles (send 0xFF × 10).
3. Assert CS0: `CONTROL = 0x02`.
4. Send commands via `DATA`, polling `STATUS` bit 0.

### Burst I/O (FIFO)
1. Read `CAP` to discover depth.
2. Set `FIFO_EN` and `FAST` bits in `CONTROL`.
3. Fill TX FIFO with command bytes.
4. Set `XFER_COUNT` with `START` bit.
5. Wait for `XFER_DONE` in `IRQ_STATUS`.

# UART — Programmer's Reference

A serial console UART, register-compatible with the industry-standard
NS16550A. Used as the hardwired boot console (`0xFF00_0000`) and as
the primary tty under NetBSD via the MI `com(4)` driver.

## Variants

| Build  | Module       | Behavior                                      |
|--------|--------------|-----------------------------------------------|
| FPGA   | `uart.sv`    | NS16550A with 16-deep TX/RX FIFOs. Acts as NS16450 when `FCR[0]=0`. |
| Sim    | `sim_uart.sv`| NS16450 (no FIFOs). Same register layout, no `FCR`/IIR FIFO bits.   |

`com(4)` detects the FIFO at probe time by writing `FCR[0]=1` and
reading `IIR[7:6]` back (`00` = 16450, `11` = 16550A FIFO active),
so both variants work transparently behind the same driver.

## Register Stride and Access Restriction

The UART is a **word-strided** variant of the NS16550A. Each
standard NS16550A 8-bit register occupies a 4-byte slot in the
address map (`reg_offset = 4 × NS16550A_offset`). Word-strided 16550A
parts are common in embedded systems (e.g. Synopsys DesignWare, TI
OMAP, Linux's `8250` driver with `reg-shift=2`) — the wider stride
matches a 32-bit memory bus directly, avoiding the need for byte-lane
muxing on every register access.

**Access restriction.** All registers must be accessed with 32-bit
aligned loads and stores only (`LDW`/`STW`). Sub-word access
(`LDB`/`LDH`/`STB`/`STH`) is not supported and yields undefined
results per the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width) rules.
Only the low 8 bits of each register slot carry data; the upper 24
bits of `wdata` are ignored on writes and read back as zero.

For NetBSD `com(4)` this maps to the FDT attachment properties
`reg-shift=2`, `reg-io-width=4`.

## Register Map

Offsets are word-strided from the device base. Register semantics
follow the standard NS16550A; only Penumbra-specific deltas are
called out below. Consult any NS16550A datasheet (e.g. National
Semiconductor's original part) for full bit-level details.

| Offset | DLAB=0       | DLAB=1 | R/W | Standard name                |
|--------|--------------|--------|-----|------------------------------|
| `0x00` | RBR / THR    | DLL    | R / W | Receive / transmit holding |
| `0x04` | IER          | DLM    | R/W | Interrupt enable             |
| `0x08` | IIR (R) / FCR (W) | —  | R/W | Interrupt ID / FIFO control  |
| `0x0C` | LCR          | —      | R/W | Line control (bit 7 = DLAB)  |
| `0x10` | MCR          | —      | R/W | Modem control (bit 3 = OUT2 = master IRQ enable) |
| `0x14` | LSR          | —      | R   | Line status                  |
| `0x18` | MSR          | —      | R   | Modem status (CTS/DSR hardwired asserted) |
| `0x1C` | SCR          | —      | R/W | Scratch register             |

### Penumbra-specific notes

- **Baud reference clock.** Software always sees a 1.8432 MHz
  reference (the canonical NS16450 crystal) regardless of the actual
  system clock. Standard NS16550A divisor math applies: `baud =
  1_843_200 / (16 × D)`, where `D = {DLM, DLL}`. Reset value is
  `D=1` → 115200 baud, ready to use without configuration.
- **IIR FIFO bits (`IIR[7:6]`).** `00` in 16450 mode, `11` when
  FIFOs are enabled. Standard 16550A behavior.
- **Character timeout.** In FIFO mode, an RX interrupt fires when
  the FIFO is non-empty and the line has been idle for ≥4 character
  times, even below the trigger threshold. Standard 16550A behavior.
- **Modem control inputs.** `CTS` and `DSR` are tied asserted in the
  current hardware — no physical modem-status pins. `RI` and `DCD`
  read as deasserted.

## Console use

The hardwired console at `0xFF00_0000` is always present (it is not
discovered via autoconfig). The boot ROM injects a synthetic
`BTAG_DEVICE` entry for it so the kernel sees a uniform device list
— see [boot-protocol.md](../boot-protocol.md).

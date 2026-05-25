# ESP32 WiFi NIC — Programmer's Reference

Bridging Ethernet frames over SLIP-framed UART using the onboard ESP32.

## Register Map

Base address assigned by autoconfig. Word-strided.

**Access restriction.** All registers are 32 bits wide and must be
accessed with 32-bit aligned loads and stores only (`LDW`/`STW`).
Sub-word access (`LDB`/`LDH`/`STB`/`STH`) is not supported and yields
undefined results per the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width) rules.

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| 0x00 | TX_DATA | W | Write byte to TX FIFO |
| 0x04 | RX_DATA | R | Read byte from RX FIFO |
| 0x08 | STATUS | R | See below |
| 0x0C | CONTROL | R/W | See below |
| 0x10 | CLKDIV | R/W | UART baud divider |
| 0x14 | TX_FRAME_END | W | Write 1 → send SLIP END delimiter |
| 0x18 | IRQ_STATUS | R | Interrupt status (read clears) |
| 0x1C | IRQ_ENABLE | R/W | Interrupt enable mask |

### STATUS Register
- Bit [0]: `TX_FULL`
- Bit [1]: `TX_EMPTY`
- Bit [2]: `RX_EMPTY`
- Bit [3]: `RX_FRAME_RDY` (complete SLIP frame available)
- Bit [5]: `ESP_READY`

### CONTROL Register
- Bit [0]: `ESP_EN` (1=run, 0=reset)
- Bit [1]: `ESP_BOOT` (drives GPIO0)
- Bit [2]: `RESET_FIFOS`

## Command Protocol (SLIP payload)

The first byte of each SLIP frame identifies the packet type:
- `0x00`: DATA (Ethernet frame follows)
- `0x01`: CMD (Request)
- `0x02`: RSP (Response)
- `0x03`: EVT (Event)

### Commands
| ID | Name | Description |
|----|------|-------------|
| 0x00 | IDENTIFY | Get MAC and version |
| 0x01 | SCAN | Scan visible networks |
| 0x02 | CONNECT | SSID + Password |
| 0x04 | STATUS | Link state and RSSI |

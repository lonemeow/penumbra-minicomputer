# ESP32 WiFi NIC — Hardware Design

## Physical Layer

### ULX3S Pin Assignments

| Signal         | FPGA Pin | Direction      | Purpose                    |
|----------------|----------|----------------|----------------------------|
| `wifi_en`      | F1       | FPGA → ESP32   | Enable (HIGH=run, LOW=reset) |
| `wifi_gpio0`   | L2       | FPGA → ESP32   | Boot mode (LOW=bootloader) |
| `wifi_txd`     | K4       | ESP32 → FPGA   | UART TX from ESP32         |
| `wifi_rxd`     | K3       | FPGA → ESP32   | UART RX to ESP32           |

### UART Bridge

The FPGA implements a SLIP-framed UART bridge with TX and RX FIFOs (2048 bytes each).

1. **SLIP codec:** Decodes incoming UART stream into packet boundaries.
2. **FIFOs:** Buffers full Ethernet frames to tolerate bursty WiFi traffic.
3. **Interrupts:** Triggered on complete SLIP frame reception.

## ESP32 Firmware

- **Scope:** station mode, raw Ethernet forwarding.
- **Protocol:** SLIP-framed packets with a 1-byte type header.
- **Build:** containerized ESP-IDF build.

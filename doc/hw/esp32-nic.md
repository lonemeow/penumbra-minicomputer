# ESP32 WiFi NIC — Design Document

The ULX3S has an onboard ESP32-WROOM-32 with a dedicated UART
connection to the FPGA.  This document describes how to use it
as a WiFi network adapter for Penumbra, bridging Ethernet frames
over SLIP-framed UART.

## Architecture Overview

```
NetBSD                Penumbra FPGA              ESP32
──────                ─────────────              ──────
pnic0 (Ethernet) ←→ NIC controller         ←→ Custom firmware
  ├─ ifconfig           slip_rx / slip_tx       WiFi bridge
  ├─ dhclient           TX/RX FIFOs             (no TCP/IP)
  ├─ route              autoconfig device       802.11 + WPA
  └─ ssh/...            IRQ to CPU              ESP-IDF, Docker build

pnicctl (userland)
  └─ scan/connect/status (via ioctl)
```

The ESP32 handles all WiFi complexity (802.11 association, WPA
handshake, encryption) internally and presents raw Ethernet frames
to the Penumbra CPU.  NetBSD runs the full TCP/IP stack and sees
a standard Ethernet interface.

## Physical Layer

### ULX3S Pin Assignments

| Signal         | FPGA Pin | Direction      | Purpose                    |
|----------------|----------|----------------|----------------------------|
| `wifi_en`      | F1       | FPGA → ESP32   | Enable (HIGH=run, LOW=reset) |
| `wifi_gpio0`   | L2       | FPGA → ESP32   | Boot mode (LOW=bootloader) |
| `wifi_txd`     | K4       | ESP32 → FPGA   | UART TX from ESP32         |
| `wifi_rxd`     | K3       | FPGA → ESP32   | UART RX to ESP32           |
| `wifi_gpio5`   | N4       | FPGA → ESP32   | LED / boot strap           |

These are independent of the FTDI console UART and the SD card
SPI pins — no conflicts.

### UART Configuration

- **Baud rate:** 921600 (configurable via ESP32 firmware and
  NIC controller CLKDIV register)
- **Format:** 8N1 (8 data bits, no parity, 1 stop bit)
- **Flow control:** none (SLIP back-pressure handles this)
- **Throughput:** ~90 KB/s raw, ~80 KB/s after SLIP framing
  overhead.  Adequate for the 12.5 MHz CPU.

The NIC controller contains a real UART TX and UART RX
(reusing the `uart_tx` pattern from `hw/rtl/fpga/`) with
a configurable baud rate divider.

## SLIP Framing

All communication uses RFC 1055 SLIP framing (already implemented
in `hw/rtl/io/slip_rx.sv` and `slip_tx.sv`).  Each SLIP frame
carries one protocol packet.

### Packet Multiplexing

The first byte after SLIP decode identifies the packet type:

| Type | Value | Direction         | Contents                          |
|------|-------|-------------------|-----------------------------------|
| DATA | 0x00  | Bidirectional     | Ethernet frame (after type byte)  |
| CMD  | 0x01  | Penumbra → ESP32  | Command request                   |
| RSP  | 0x02  | ESP32 → Penumbra  | Command response                  |
| EVT  | 0x03  | ESP32 → Penumbra  | Asynchronous event                |

Data frames are Ethernet II frames (dest MAC + src MAC + EtherType
+ payload), prepended with the 0x00 type byte.  Maximum Ethernet
frame is 1514 bytes, so maximum SLIP payload is 1515 bytes
(+1 type byte), worst-case SLIP-encoded ~1521 bytes (rare,
only if frame contains many 0xC0/0xDB bytes).

## Command Protocol

Commands are synchronous request/response pairs.  The NIC
controller (or driver) sends a CMD packet; the ESP32 responds
with a RSP packet.  Only one command may be outstanding at a time.

### Command Format

```
CMD packet:  [0x01] [cmd_id:8] [seq:8] [payload...]
RSP packet:  [0x02] [cmd_id:8] [seq:8] [status:8] [payload...]
EVT packet:  [0x03] [evt_id:8] [payload...]
```

`seq` is a sequence number (incremented per command) to match
responses.  `status` is 0 for success, nonzero for error.

### Commands

| cmd_id | Name       | Request payload         | Response payload              |
|--------|------------|-------------------------|-------------------------------|
| 0x00   | IDENTIFY   | (none)                  | magic(4) version(2) mac(6)    |
| 0x01   | SCAN       | (none)                  | count(1) then scan entries    |
| 0x02   | CONNECT    | ssid_len(1) ssid(N) pass_len(1) pass(N) | (none)     |
| 0x03   | DISCONNECT | (none)                  | (none)                        |
| 0x04   | STATUS     | (none)                  | state(1) rssi(1) ssid_len(1) ssid(N) |
| 0x05   | SET_BAUD   | baud_rate(4 LE)         | (none, takes effect after RSP)|
| 0x06   | SET_MAC    | mac(6)                  | (none)                        |

**IDENTIFY** response magic: `0x504E4943` ("PNIC" in LE).
Version: major.minor (e.g., 0x0100 = v1.0).  The 6-byte MAC
address allows the driver to learn the interface MAC at probe
time.

**SCAN** response entries: `[rssi:8] [channel:8] [security:8]
[ssid_len:8] [ssid:N]`.  Security flags: bit 0 = open,
bit 1 = WPA, bit 2 = WPA2, bit 3 = WPA3.

**STATUS** states: 0 = disconnected, 1 = connecting,
2 = connected, 3 = error.

### Events

| evt_id | Name       | Payload            |
|--------|------------|--------------------|
| 0x00   | LINK_UP    | (none)             |
| 0x01   | LINK_DOWN  | reason(1)          |

Events are unsolicited — the ESP32 sends them when WiFi
state changes.  The NIC controller generates an interrupt
on event receipt.

## FPGA Hardware

### NIC Controller Module (`pnic.sv`)

The NIC controller wraps the SLIP codec, UART, and FIFOs into
a bus-attached autoconfigurable device.

```
                         pnic.sv
┌──────────────────────────────────────────────────┐
│                                                  │
│  Bus interface ──→ Register file                 │
│                      ├─ TX_DATA (W)              │
│                      ├─ RX_DATA (R)              │
│                      ├─ STATUS  (R)              │
│                      ├─ CONTROL (R/W)            │
│                      ├─ CLKDIV  (R/W)            │
│                      └─ IRQ_*   (R/W)            │
│                                                  │
│  TX path:  TX_DATA → TX FIFO → slip_tx → UART TX → wifi_rxd │
│  RX path:  wifi_txd → UART RX → slip_rx → RX FIFO → RX_DATA │
│                                                  │
│  Frame logic:                                    │
│    slip_rx.o_frame_end → increment RX frame count│
│    TX_FRAME_END reg → slip_tx.i_frame_end        │
│                                                  │
│  IRQ: rx_frame_ready | link_event                │
│                                                  │
└──────────────────────────────────────────────────┘
```

### Register Map

Base address assigned by autoconfig.  Word-strided (offset
step = 4), data in low bits.

| Offset | Name         | R/W | Bits | Description                           |
|--------|--------------|-----|------|---------------------------------------|
| 0x00   | TX_DATA      | W   | 7:0  | Write byte to TX FIFO                 |
| 0x04   | RX_DATA      | R   | 7:0  | Read byte from RX FIFO                |
| 0x08   | STATUS       | R   | 7:0  | See below                             |
| 0x0C   | CONTROL      | R/W | 7:0  | See below                             |
| 0x10   | CLKDIV       | R/W | 15:0 | UART baud divider                     |
| 0x14   | TX_FRAME_END | W   | 0    | Write 1 → send SLIP END delimiter     |
| 0x18   | IRQ_STATUS   | R   | 7:0  | Interrupt status (read clears)        |
| 0x1C   | IRQ_ENABLE   | R/W | 7:0  | Interrupt enable mask                 |

**STATUS register:**

| Bit | Name          | Description                         |
|-----|---------------|-------------------------------------|
| 0   | TX_FULL       | TX FIFO is full                     |
| 1   | TX_EMPTY      | TX FIFO is empty                    |
| 2   | RX_EMPTY      | RX FIFO is empty                    |
| 3   | RX_FRAME_RDY  | Complete frame available in RX FIFO |
| 4   | SLIP_TX_BUSY  | SLIP encoder inserting escape       |
| 5   | ESP_READY     | ESP32 enable is asserted            |

**CONTROL register:**

| Bit | Name          | Description                         |
|-----|---------------|-------------------------------------|
| 0   | ESP_EN        | ESP32 enable (drives `wifi_en`)     |
| 1   | ESP_BOOT      | ESP32 boot mode (drives `wifi_gpio0`, active low) |
| 2   | RESET_FIFOS   | Pulse: clear TX and RX FIFOs        |

**IRQ_STATUS / IRQ_ENABLE:**

| Bit | Name          | Description                         |
|-----|---------------|-------------------------------------|
| 0   | RX_FRAME      | Complete SLIP frame received        |
| 1   | TX_EMPTY      | TX FIFO drained (frame fully sent)  |

### Autoconfig Identity

| Field     | Value              |
|-----------|--------------------|
| CLASS     | `ACFG_CLASS_NIC` (5) — new class code |
| SIZE      | 4096 (one page)    |
| ID        | `ACFG_NIC_PNIC` (1) — ESP32 SLIP NIC  |
| NAME      | "PNIC" (+ null pad)|

The driver matches on **both** class and ID: `pb_class == NIC &&
pb_id == NIC_PNIC`.  This allows future NIC devices (e.g., Wiznet
W5500 with `ACFG_NIC_W5500 = 2`) to use the same class code with
a different driver binding.

### FIFOs

TX and RX FIFOs are 2048 bytes each (sufficient for one
full Ethernet frame + SLIP overhead + some buffering).
Implemented as dual-port BRAM (ECP5 has plenty).

The RX FIFO includes a frame boundary marker.  When
`slip_rx.o_frame_end` fires, the current write pointer
is recorded as a frame boundary.  `RX_FRAME_RDY` asserts
when at least one complete frame is in the FIFO.  The
driver reads bytes until it hits the boundary, then
processes the complete frame.

Alternatively, the RX side can prepend a 2-byte LE length
to each frame in the FIFO (written when `o_frame_end`
fires), so the driver knows how many bytes to read.
This avoids needing a sideband FIFO for frame boundaries.

### TX Flow

1. Driver writes Ethernet frame bytes to TX_DATA
   (type byte 0x00 first, then frame)
2. Driver writes 1 to TX_FRAME_END
3. Hardware: FIFO drains through `slip_tx` → UART TX
4. TX_EMPTY interrupt fires when complete

### RX Flow

1. UART RX → `slip_rx` decodes SLIP framing
2. Decoded bytes written to RX FIFO
3. On `o_frame_end`, frame boundary recorded
4. RX_FRAME interrupt fires
5. Driver reads frame from RX_DATA (type byte first)
6. Type 0x00 → Ethernet frame → pass to `if_input`
7. Type 0x02/0x03 → command response / event → handle

## ESP32 Firmware

### Scope

Minimal ESP-IDF application.  No lwIP, no TCP/IP, no NVS
(credentials come from the host, not stored on ESP32).

Responsibilities:
- Initialize WiFi in station mode
- Accept CONNECT/DISCONNECT/SCAN/STATUS commands over UART
- Forward Ethernet frames bidirectionally (WiFi ↔ UART+SLIP)
- Send LINK_UP/LINK_DOWN events on WiFi state changes

### Build

Containerized ESP-IDF build, no host installation required:

```sh
# Build
docker run --rm -v $PWD/esp32:/project -w /project \
    espressif/idf:v5.4 idf.py build

# Output: esp32/build/pnic-firmware.bin
```

A wrapper script in `esp32/tools/` (following the same pattern
as `hw/tools/oss-cad-suite/`) makes this transparent.

### Flash

User flashes once from a PC with USB access to the ULX3S:

```sh
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 \
    write_flash 0x0 pnic-firmware.bin
```

The `wifi_en` and `wifi_gpio0` pins are directly connected
to the FPGA for in-circuit programming support if we decide
to add that later.

### Source Layout

```
esp32/
  CMakeLists.txt          # ESP-IDF project file
  main/
    main.c                # app_main(), init, UART task
    slip.c / slip.h       # SLIP encode/decode (software)
    protocol.c / protocol.h  # Command handling
    bridge.c / bridge.h   # WiFi RX callback, TX path
  sdkconfig.defaults      # Minimal config (no lwIP, etc.)
  tools/
    docker-build.sh       # Build wrapper
    flash.sh              # Flash wrapper
```

### ESP32 UART Configuration

The ESP32's UART0 (GPIO1 TX, GPIO3 RX) is the one connected
to `wifi_txd`/`wifi_rxd` on the FPGA.  The firmware initializes
UART0 at 921600 baud, 8N1.

**Important:** The ESP32 ROM bootloader also uses UART0 and
prints boot messages at 115200 baud on reset.  The NIC
controller must tolerate garbage bytes during ESP32 boot
(the SLIP decoder handles this naturally — random bytes
without END delimiters are just noise).  The driver should
wait for a successful IDENTIFY response before declaring
the device ready.

### SET_BAUD Handshake

Both sides start at 921600.  If the driver sends SET_BAUD
to change speed:
1. Driver sends SET_BAUD command at current baud rate
2. ESP32 sends RSP at current baud rate
3. ESP32 switches to new baud rate after a brief delay
4. Driver updates CLKDIV register to match
5. Driver sends IDENTIFY at new baud rate to confirm

## NetBSD Driver (`pnic(4)`)

### Attachment

```
pnic0 at pbbus0: PNIC (ESP32 WiFi NIC) at 0xff002000
pnic0: PNIC firmware v1.0, address 24:6f:28:xx:xx:xx
pnic0: Ethernet address 24:6f:28:xx:xx:xx
```

The driver matches `pb_class == ACFG_CLASS_NIC && pb_id ==
ACFG_NIC_PNIC` on pbbus.  At attach:
1. Map device registers via `bus_space`
2. Assert ESP_EN in CONTROL register (release ESP32 from reset)
3. Wait for ESP32 boot (~500ms)
4. Send IDENTIFY command, verify magic and version
5. If no response or wrong magic: `aprint_error("ESP32
   firmware not installed or not responding\n")`, don't
   attach the network interface
6. On success: read MAC address, call `if_attach` +
   `ether_ifattach`
7. Register interrupt handler for RX_FRAME

### Interface Operations

| Operation          | Implementation                          |
|--------------------|-----------------------------------------|
| `if_init`          | Enable IRQs, send STATUS to ESP32       |
| `if_start` (TX)    | Dequeue mbuf, write to TX_DATA + END    |
| `if_ioctl`         | Standard Ethernet + PNIC custom ioctls  |
| `if_stop`          | Disable IRQs, optionally DISCONNECT     |
| RX interrupt       | Read frame from RX_DATA, `if_input`     |

### Custom ioctls

| ioctl              | Purpose                                 |
|--------------------|-----------------------------------------|
| `PNIC_SCAN`        | Trigger WiFi scan, return results       |
| `PNIC_CONNECT`     | Associate with AP (SSID + password)     |
| `PNIC_DISCONNECT`  | Disassociate                            |
| `PNIC_STATUS`      | Return link state, RSSI, current SSID   |

### Polling Fallback

The PNIC controller asserts the shared `/IRQ` line when frames
are available (IRQ status register, like all bus devices).  As
a fallback, the driver can also use polled mode with a callout
(like `com(4)` does with `sc_poll_ticks`).  Poll STATUS register
for RX_FRAME_RDY every N ticks.

## Userland Tool (`pnicctl`)

Simple utility that opens `/dev/pnic0` (or uses ioctl on
the network interface) to send WiFi management commands.

```
Usage:
  pnicctl scan                    # List visible networks
  pnicctl connect SSID [PASSWORD] # Associate with AP
  pnicctl disconnect              # Disassociate
  pnicctl status                  # Show link state, RSSI, SSID
```

### Boot-Time Configuration

`/etc/pnic.conf`:
```
ssid=MyNetwork
password=secret
```

`/etc/rc.d/pnic` (rc.d script):
```sh
pnicctl connect "$(get_conf ssid)" "$(get_conf password)"
dhclient pnic0
```

## Simulation

For ISS and RTL simulation without a real ESP32, the testbench
provides an ESP32 emulator (similar to `sd_card_sim.h` for the
SD card).

The emulator:
- Responds to IDENTIFY with a valid magic + version + MAC
- Accepts CONNECT (always succeeds after a short delay)
- Echoes or generates test Ethernet frames on request
- Sends LINK_UP event after CONNECT

This allows full-stack testing: NetBSD boots, `pnic(4)` attaches,
`pnicctl connect` succeeds, driver can send/receive frames.

## Implementation Order

1. **SLIP testbenches** — verify `slip_rx` and `slip_tx` with
   edge cases (consecutive escapes, empty frames, max-size
   frames, invalid escapes)

2. **NIC controller RTL** (`pnic.sv`) — register file, TX/RX
   FIFOs, SLIP integration, UART TX/RX, autoconfig wrapper.
   Test with Verilator testbench using direct stimulus.

3. **ESP32 emulator** (`esp32_sim.h`) — testbench-side UART
   responder for IDENTIFY, CONNECT, data echo.  Integrate
   with `machine_sim.sv`.

4. **FPGA integration** — add `pnic.sv` to `ulx3s_top.sv`,
   wire to `wifi_en`/`wifi_gpio0`/`wifi_txd`/`wifi_rxd`.

5. **ESP32 firmware** — minimal ESP-IDF project, Docker build,
   flash instructions.

6. **Hardware test** — FPGA + ESP32 talking over UART, verify
   SLIP framing and IDENTIFY handshake.

7. **NetBSD driver** (`pnic(4)`) — pbbus attachment,
   register access, IDENTIFY probe, `if_attach`,
   TX/RX frame path, custom ioctls.

8. **Userland tool** (`pnicctl`) — scan, connect, status.

9. **End-to-end test** — boot NetBSD on FPGA, `pnicctl connect`,
   `dhclient pnic0`, `ping`.

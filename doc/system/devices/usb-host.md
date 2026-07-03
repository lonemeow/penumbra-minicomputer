# USB Host Controller — Programmer's Reference

A USB host controller for the Penumbra bus (`CLASS_USBHC`). Software
issues one USB transaction at a time — a token packet, an optional data
packet, and a handshake — and the controller's serial engine handles the
line-rate work (NRZI, bit-stuffing, CRC, and handshake timing) that
software cannot meet. Everything above a transaction — device
enumeration, descriptor parsing, HID handling — is the host software's
job, not the controller's.

This document defines the **`CLASS_USBHC` minimum protocol**: the
transaction-level register interface every device of the class implements
after reset. A device may add richer capabilities (a DMA
transfer-descriptor engine, downstream-hub support) advertised via
`CFG_ID`, but must always present this interface after reset — so generic
firmware and a single class-bound OS driver work against any conformant
controller.

## Register Interface

Word-strided registers and a packet-data buffer. Base assigned by
autoconfig.

**Access.** 32-bit aligned `LDW`/`STW` only; no byte-lane enables — the
same word-strided convention as the other Penumbra devices. See the bus
protocol's [Access Width](../../hardware/bus-protocol.md#access-width).

| Offset      | Name          | R/W   | Description                          |
|-------------|---------------|-------|--------------------------------------|
| `0x00`      | `CAP`         | R     | Version, data-buffer size, speeds    |
| `0x04`      | `IRQ_STATUS`  | R/W1C | Interrupt sources                    |
| `0x08`      | `IRQ_ENABLE`  | R/W   | Interrupt mask                       |
| `0x0C`      | `PORT_STATUS` | R     | Connect, speed, line state           |
| `0x10`      | `PORT_CTRL`   | R/W   | Power, reset, run                    |
| `0x14`      | `FRAME`       | R     | Frame counter                        |
| `0x18`      | `TOKEN`       | R/W   | Transaction token                    |
| `0x1C`      | `XFER_CTRL`   | W     | Transaction length + start           |
| `0x20`      | `XFER_STATUS` | R     | Transaction result + received length |
| `0x40…0x7C` | `DATA`        | R/W   | Packet data buffer                   |

### Register Details

#### CAP (0x00)
- Bits [7:0]: Version (`1`)
- Bits [15:8]: `DATA` buffer size in bytes
- Bit [16]: `LS` — low-speed (1.5 Mbps) supported
- Bit [17]: `FS` — full-speed (12 Mbps) supported

#### IRQ_STATUS (0x04) — write-1-to-clear
- Bit [0]: `XFER_DONE` — a transaction completed
- Bit [1]: `PORT_CHANGE` — port status changed (connect / disconnect / reset complete)
- Bit [2]: `SOF` — frame marker elapsed

#### IRQ_ENABLE (0x08)
Per-source mask, same bit positions as `IRQ_STATUS`.

#### PORT_STATUS (0x0C)
- Bit [0]: `CONNECT` — a device is attached
- Bit [1]: `ENABLED` — port enabled (after reset)
- Bit [2]: `RESET_ACTIVE` — bus reset being driven
- Bit [3]: `SUSPENDED`
- Bits [5:4]: `SPEED` — 0 none, 1 low, 2 full
- Bits [9:8]: `LINE` — raw D+/D- line state (J / K / SE0)

#### PORT_CTRL (0x10)
- Bit [0]: `POWER` — enable port power
- Bit [1]: `RESET` — drive a bus reset (SE0) while set
- Bit [2]: `RUN` — generate frame markers and accept transactions
- Bit [3]: `SUSPEND`
- Bit [4]: `RESUME`

#### FRAME (0x14)
- Bits [10:0]: `FRAME_NUM` — increments each 1 ms frame marker

#### TOKEN (0x18)
- Bits [1:0]: `PID` — 0 SETUP, 1 OUT, 2 IN
- Bits [10:4]: `DEVADDR` — target device address (0–127)
- Bits [14:11]: `ENDPOINT` — target endpoint (0–15)
- Bit [16]: `TOGGLE` — data toggle (DATA0 / DATA1)

#### XFER_CTRL (0x1C) — writing starts a transaction
- Bits [6:0]: `LENGTH` — bytes to send (`OUT`/`SETUP`) or accept (`IN`)
- Bit [16]: `START` — launch the transaction described by `TOKEN`

#### XFER_STATUS (0x20)
- Bit [0]: `DONE` — transaction complete (mirrors `IRQ_STATUS.XFER_DONE`)
- Bits [3:1]: `RESULT` — 0 ACK, 1 NAK, 2 STALL, 3 TIMEOUT, 4 ERROR (CRC / bit-stuff), 5 OVERFLOW
- Bit [4]: `RXTOGGLE` — data toggle of the DATAx packet received on an `IN`
- Bits [14:8]: `RXLEN` — bytes received into `DATA` on an `IN`

The controller acknowledges any CRC-good `IN` data packet and reports the
toggle it carried in `RXTOGGLE`; it applies no toggle policy of its own.
Software compares `RXTOGGLE` against the toggle it expected: a mismatch
is the device retransmitting a packet whose handshake it lost, and the
data must be discarded — the acknowledge alone resynchronizes the device.

#### DATA (0x40 …)
The packet payload, accessed as little-endian words (four bytes per
slot). `CAP` reports the size; it is large enough for the maximum packet
of the supported speeds (8 bytes low-speed, 64 bytes full-speed). For an
`OUT`/`SETUP` the software fills `DATA` before starting; for an `IN` it
reads `RXLEN` bytes after `DONE`. On an `IN` the controller stores the
received packet body as it arrives, so the packet's two trailing CRC
bytes may follow the payload in the buffer — `RXLEN` excludes them, and
buffer content beyond `RXLEN` is not meaningful.

## Transaction Model

One transaction = a token, an optional data packet, and a handshake,
executed atomically by the controller:

1. For `OUT`/`SETUP`, write the payload to `DATA`.
2. Set `TOKEN` (PID, address, endpoint, toggle).
3. Write `XFER_CTRL = LENGTH | START`.
4. Wait for `XFER_DONE` (poll `XFER_STATUS.DONE` or take the interrupt).
5. Read `RESULT`; on `ACK` for an `IN`, read `RXLEN` bytes from `DATA`.

Higher-level USB transfers compose from transactions — the controller has
no notion of them:

- **Control transfer** — a `SETUP` transaction (8-byte request, toggle
  DATA0), an optional data stage (one or more `IN`/`OUT`, alternating the
  toggle), then a zero-length status stage in the opposite direction.
- **Interrupt-IN poll** — one `IN` transaction at the endpoint's interval.
  `NAK` means no new data (e.g., an idle keyboard); `ACK` returns `RXLEN`
  bytes (e.g., an 8-byte HID boot report).

## Port and Frame Timing

`PORT_STATUS` / `PORT_CTRL` are the controller's single downstream port.
Host software emulates a one-port USB root hub over them: apply `POWER`,
watch `CONNECT` (via `PORT_CHANGE`), drive `RESET`, read `SPEED`, then run
transactions. While `PORT_CTRL.RUN` is set the controller emits the 1 ms
frame marker (full-speed SOF / low-speed keep-alive) so the device does
not suspend; `FRAME` counts markers and the `SOF` interrupt lets software
pace periodic polling.

## Scope of the Minimum

The minimum protocol covers **low- and full-speed, a single
directly-attached device** (no downstream hub, so no split/PRE handling),
and **control and interrupt transfers** — sufficient to enumerate and
poll a boot-protocol HID keyboard. Bulk and isochronous transfers,
downstream-hub support, and DMA descriptor engines, where present, are
richer features behind `CFG_ID`.

## Relationship to Host Software

The interface is defined so generic firmware (the boot ROM) can enumerate
and poll a USB boot keyboard using only these registers — the USB analog
of the ROM reading a [UART](uart.md) for console input. Under NetBSD, a
single host-controller driver binds to `CLASS_USBHC` and implements the
machine-independent USB stack's bus interface; that layer performs
enumeration, descriptor parsing, and HID handling above it, exactly as it
does for any other controller. A `ukbd` keyboard discovered this way
drives `wskbd`, which together with a [text-video](text-video.md) or
framebuffer console forms a `wscons` local console.

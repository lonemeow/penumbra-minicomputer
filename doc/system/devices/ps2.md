# PS/2 Port — Programmer's Reference

A port for one PS/2 device, a keyboard or a pointing device, on the
Penumbra bus (`CLASS_PS2`). The port exchanges single bytes with the
attached device and checks each one it receives. Everything above a
byte is the host software's job: which kind of device is attached,
its commands and replies, scan codes and their translation, key
repeat, indicator state.

This document defines the **`CLASS_PS2` minimum protocol**: the
registers every device of the class implements, and the state it
powers up in, after reset. Bytes are the class's identity: a device
that hands over key codes of its own, or characters, is not a
`CLASS_PS2` device. A machine with two sockets presents two devices
of the class, and software asks each what is attached to it.

## The Link

A PS/2 link is two wires, clock and data, that either end can pull
low and neither drives high. The attached device drives the clock for
bytes in both directions. The port never clocks the link: all it does
to the clock line is pull it low and hold it there. Held low, the
line tells the device to wait: the device sends nothing, keeps what
it has to send, and sends it once the line is released. The port uses
this in two ways, to make the device wait while software has not
taken the bytes already received (see [Receive](#receive)), and to
claim the link for a byte of its own (see [Transmit](#transmit)).

Each byte travels in a frame with a start bit, a parity bit and a
stop bit, which the receiving end checks. The link has no signal for
presence: a port with nothing attached looks like one whose device is
idle, until software sends a command and nothing answers it.

The port may be a socket, or a bridge that presents another kind of
keyboard as a PS/2 device. Software cannot tell which, and has no
need to.

## Register Interface

Six word-strided registers. Base address assigned by autoconfig.

**Access.** The registers are accessed with 32-bit aligned loads and
stores only (`LDW`/`STW`); the device implements no byte-lane enables
for them. See the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width).

| Offset | Name         | R/W | Description                                             |
|--------|--------------|-----|---------------------------------------------------------|
| `0x00` | `CAP`        | R   | Version, optional features                              |
| `0x04` | `IRQ_STATUS` | R   | Interrupt conditions                                    |
| `0x08` | `IRQ_ENABLE` | R/W | Per-source interrupt mask                               |
| `0x0C` | `CTRL`       | R/W | Port enable                                             |
| `0x10` | `RX_DATA`    | R   | Oldest received byte and its flags; reading releases it |
| `0x14` | `TX_DATA`    | R/W | Write: byte to send. Read: state of the transmission    |

## Register Details

### CAP (0x00)

- Bits [7:0]: Version (`1`)

Remaining bits are reserved for the class's optional features, one
bit each; none is defined, and they read 0. Capabilities beyond the
class's own are advertised through `CFG_ID`.

### IRQ_STATUS (0x04) / IRQ_ENABLE (0x08)

- Bit [0]: `RX` — a received byte waits in `RX_DATA`
- Bit [1]: `TX` — the port can take a byte to send: it is enabled and
  no transmission is in progress

Remaining bits reserved (read 0).

Both bits are conditions, not events, and `IRQ_STATUS` is read-only.
`RX` ends when software reads the last waiting byte and `TX` when it
writes `TX_DATA`, so there is nothing to acknowledge. `TX` is
asserted whenever the port is idle, so a driver enables it only while
it has a byte waiting to go.

`IRQ_ENABLE` uses the same layout and masks which sources assert the
device's interrupt output; `IRQ_STATUS` shows each source's condition
whether or not it is enabled. `IRQ_ENABLE` resets to `0`.

The device drives the shared bus interrupt line, so a handler must
determine from `IRQ_STATUS` whether this device is asserting, and must
not assume it is — see [Interrupt Model](../bus.md#interrupt-model).

### CTRL (0x0C)

- Bit [0]: `EN` — the port is in use. While clear, the port holds the
  device off, receives nothing and sends nothing. Clearing it discards
  any byte waiting in `RX_DATA` and abandons a transmission in
  progress. Resets to `0`.

Remaining bits reserved (read 0).

### RX_DATA (0x10)

- Bits [7:0]: `BYTE` — the oldest received byte
- Bit [8]: `VALID` — `BYTE` is a received byte software has not taken
- Bit [9]: `ERR` — the byte's start, parity or stop bit was wrong, and
  `BYTE` holds the bits as they arrived

Remaining bits reserved (read 0).

A read with `VALID` set takes the byte: the next read presents the
one behind it, or `VALID` clear. A read with `VALID` clear returns 0
and changes nothing. Writes are ignored.

### TX_DATA (0x14)

Written:

- Bits [7:0]: `BYTE` — the byte to send. The write starts a
  transmission; it is ignored while `BUSY` is set or `CTRL.EN` is
  clear.

Read:

- Bit [8]: `BUSY` — a byte written is still being sent
- Bit [9]: `ERR` — the most recent byte written was not delivered.
  Cleared by the next write that starts a transmission.

Remaining bits read 0, and so do both flags while `CTRL.EN` is clear.

## Receive

The port holds the bytes it has received in the order they arrived,
and `RX_DATA` presents the oldest. Every port holds at least one.
When it has room for no more, it holds the device off until software
reads one, so no byte is lost to a slow reader and the port has no
overrun to report. A device held off for long enough may overflow a
buffer of its own. A keyboard reports that in its byte stream, as its
command set defines; a bridge may drop what does not fit and report
nothing.

A byte whose start, parity or stop bit is wrong is delivered with
`ERR` set rather than dropped, since the remedy is software's:
keyboards and pointing devices both define a command (`0xFE`) that
asks for the last byte again. A frame the device stops sending
partway is discarded.

## Transmit

Writing `TX_DATA` claims the link and offers the byte to the device,
which clocks it in and acknowledges it on the wire. `BUSY` is set
from the write until the device has acknowledged the byte or the port
has given up, which it does when the device does not start clocking,
stops partway, or does not acknowledge. Either way `BUSY` clears
within 20 ms of the write, and `ERR` then says which happened.

`ERR` clear means the byte was clocked in and acknowledged on the
wire, and no more: whether the device acts on it shows only in what
it sends back. With nothing on the link every transmission ends in
`ERR`. A bridge is on the link whether or not a keyboard is behind
it, so there only an answer to a command shows a device.

Sending takes precedence over receiving. A byte the device had begun
to send when the port claimed the link is abandoned by the device and
sent again afterwards, as the protocol requires of it. Bytes already
received are unaffected and stay until read. What the device sends in
reply, its acknowledgement code included, arrives as ordinary
received bytes behind any still waiting, so software that sends with
bytes unread reads those first.

## Attachment and Removal

A PS/2 device announces itself when it powers up: it runs a self-test
and sends the result (`0xAA` for a pass), and a pointing device
follows that with its identity. Received unprompted, the result means
a device has just been attached and is in its power-on state,
whatever software had configured before. Removal is silent: software
that needs to know sends a command and waits for its answer.

## Driver Flow

### Bring-up — the class minimum

1. Set `CTRL.EN`.
2. Read `RX_DATA`, discarding what was waiting, until no byte has
   arrived for 5 ms. A device that was held off kept what it had to
   send, through a reset of the machine too, and hands it over a
   frame at a time once the port reads.
3. Send a command the device answers (its reset command serves) and
   wait for the answer to learn whether anything is attached.
   `TX_DATA.ERR` set says at once that nothing is on the link.

What the replies mean belongs to the device's command set, not to the
port. A keyboard needs no command to start reporting keys, so a
consumer that only reads one can stop after step 2.

### Send

1. Wait until `TX_DATA.BUSY` is clear.
2. Write the byte to `TX_DATA`.
3. Wait until `BUSY` is clear again, at most 20 ms. `ERR` set means
   the device did not take the byte.

### Receive, polled

1. Read `RX_DATA`. `VALID` clear means nothing has arrived.
2. Otherwise use `BYTE`, or ask the device for it again if `ERR` is
   set.

### Interrupts

1. Set `RX` in `IRQ_ENABLE`.
2. On interrupt, read `IRQ_STATUS`. If no enabled bit is set, the
   interrupt is another device's.
3. For `RX`, read `RX_DATA` until `VALID` is clear.
4. A driver that queues bytes to send sets `TX` in `IRQ_ENABLE` while
   its queue is not empty. On `TX` it writes the next byte, or clears
   `TX` in `IRQ_ENABLE` when none is left.

## Scope of the Minimum

The minimum protocol is the link itself: bytes in both directions,
the receiving end's checks, and the hold that paces the device.
Nothing in it depends on what is attached.

Translation between scan code sets, which the PC's keyboard
controller offers, and several devices multiplexed onto one port,
where present, are richer features behind `CFG_ID`.

## Relationship to Host Software

The interface is defined so that generic firmware can read a keyboard
by polling, with `CTRL` and `RX_DATA` alone, and so that one
class-bound driver serves every conformant port. Under NetBSD that
driver is the controller half of `pckbport(9)`: it supplies the port
operations (fetch a byte, send one, enable the port, switch between
interrupts and polling), and the machine-independent `pckbd(4)` and
`pms(4)` attach above it. The port translates nothing, so the driver
reports scan code translation as unavailable and `pckbd` converts
scan code set 2 itself. A keyboard found this way drives `wskbd`,
pairing with a [display](display.md) console to form a `wscons` local
console.

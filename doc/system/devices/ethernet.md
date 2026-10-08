# Ethernet Adapter — Programmer's Reference

An Ethernet adapter for the Penumbra bus (`CLASS_ETHERNET`). The
device sends and receives whole Ethernet frames, and holds them in
memory of its own: fixed-size slots, arranged as a receive ring and a
transmit ring, that software reads and writes over the bus. It never
reads or writes system memory (see the bus protocol's
[One Master](../../hardware/bus-protocol.md#one-master)). Everything
above a frame, from ARP upward, is the host software's job.

This document defines the **`CLASS_ETHERNET` minimum protocol**: the
registers every device of the class implements, and the state it
powers up in, after reset. The mandatory core is small: one receive
slot and one transmit slot, full duplex, and a filter on one station
address. What differs between devices is reported rather than
assumed: how many slots each ring has and how large they are, which
speeds the device carries, whether software manages a PHY through the
device, and whether the device knows a station address of its own.
Whole frames exchanged through slot rings are the class's identity: a
device that hands over a byte stream, or that terminates protocols
above the frame, is not a `CLASS_ETHERNET` device.

## Register Interface

A register block followed by two slot apertures. Base address assigned
by autoconfig.

**Access.** The registers are accessed with 32-bit aligned loads and
stores only (`LDW`/`STW`); the device implements no byte-lane enables
for them, and every register is *word-strided*. The two slot apertures
are the exception: they follow the bus's default access contract, as
plain memory does (see [Slots](#slots)). See the bus protocol's
[Access Width](../../hardware/bus-protocol.md#access-width).

| Offset    | Name         | R/W   | Description                                        |
|-----------|--------------|-------|----------------------------------------------------|
| `0x000`   | `CAP`        | R     | Version, link management, station address, speeds  |
| `0x004`   | `IRQ_STATUS` | R     | Interrupt sources                                  |
| `0x008`   | `IRQ_ENABLE` | R/W   | Interrupt mask, `IRQ_STATUS` layout                |
| `0x00C`   | `CTRL`       | R/W   | Receive and transmit enable, filter modes, speed   |
| `0x010`   | `LINK`       | R     | Link state, on a device that manages its own link  |
| `0x014`   | `MDIO`       | R/W   | PHY management transaction (optional)              |
| `0x018`   | `ADDR_LO`    | R/W   | Station address, bytes 0–3                         |
| `0x01C`   | `ADDR_HI`    | R/W   | Station address, bytes 4–5                         |
| `0x020`   | `FACTORY_LO` | R     | Factory station address, bytes 0–3 (optional)      |
| `0x024`   | `FACTORY_HI` | R     | Factory station address, bytes 4–5, and validity (optional) |
| `0x040`   | `RX_GEOM`    | R     | Receive ring: slot count and slot size             |
| `0x044`   | `RX_PROD`    | R     | Receive ring: frames the device has stored         |
| `0x048`   | `RX_CONS`    | R/W   | Receive ring: frames software has released         |
| `0x04C`   | `RX_DROP`    | R     | Frames discarded because the receive ring was full |
| `0x050`   | `TX_GEOM`    | R     | Transmit ring: slot count and slot size            |
| `0x054`   | `TX_PROD`    | R/W   | Transmit ring: frames software has queued          |
| `0x058`   | `TX_CONS`    | R     | Transmit ring: frames the device has finished with |
| `0x05C`   | `TX_WM`      | R/W   | Transmit ring: queued-frame threshold for the `TX` source |
| `0x8000…` | `TX`         | W     | Transmit slot aperture                             |
| `0x10000…`| `RX`         | R     | Receive slot aperture                              |

## Register Details

### CAP (0x000)

- Bits [7:0]: Version (`1`)
- Bit [8]: `MDIO` — the device's link is a PHY that software manages
  through [`MDIO`](#mdio-0x014), and software tells the device the
  link's speed in [`CTRL.SPEED`](#ctrl-0x00c). When clear, the device
  manages its own link and reports it in [`LINK`](#link-0x010).
- Bit [9]: `ADDR` — the device may hold a station address of its own,
  read from `FACTORY_LO` and `FACTORY_HI`.
- Bits [18:16]: `SPEEDS` — one bit per speed the device carries: bit 16
  for 10 Mbit/s, bit 17 for 100, bit 18 for 1000. At least one is set.

Remaining bits reserved (read 0); further capabilities are advertised
through `CFG_ID`.

`MDIO` is the one bit no consumer can ignore, because it says who
brings the link up: see [Link Management](#link-management). The rest
describe resources. A consumer that never reads `ADDR` supplies an
address itself, and one that never reads `SPEEDS` still works on a
device that manages its own link.

### IRQ_STATUS (0x004) / IRQ_ENABLE (0x008)

- Bit [0]: `RX` — the receive ring holds frames software has not
  released
- Bit [1]: `TX` — the transmit ring holds fewer frames than `TX_WM`

Remaining bits reserved (read 0).

`RX` interrupt is asserted when there is at least one received frame not
consumed (i.e. `RX_PROD` is not equal to `RX_CONS`).

`TX` interrupt is asserted when the number of frames queued for
transmission (`TX_PROD - TX_CONS`) is less than `TX_WM`.

Both bits are conditions, not events, and `IRQ_STATUS` is read-only.
A source ends when software changes what it is compared with, by
releasing frames through `RX_CONS` or by writing `TX_WM`, so there is
nothing to acknowledge. A full receive ring needs no source of its
own: `RX` is asserted whenever the ring holds anything, and the frames
lost to a full one are counted in [`RX_DROP`](#rx_drop-0x04c). A
change of link raises no interrupt either (see
[Link Management](#link-management)).

`IRQ_ENABLE` uses the same layout and masks which sources assert the
device's interrupt output; `IRQ_STATUS` shows each source's condition
whether or not it is enabled. `IRQ_ENABLE` resets to `0`.

The device drives the shared bus interrupt line, so a handler must
determine from `IRQ_STATUS` whether this device is asserting, and must
not assume it is — see [Interrupt Model](../bus.md#interrupt-model).

### CTRL (0x00C)

- Bit [0]: `RX_EN` — receive frames into the receive ring.
- Bit [1]: `TX_EN` — send the frames queued in the transmit ring.
- Bit [2]: `PROMISC` — accept every frame, whatever its destination.
- Bit [3]: `ALLMULTI` — accept every frame sent to a group address.
- Bits [9:8]: `SPEED` — the speed the device sends and receives at:
  `0` for 10 Mbit/s, `1` for 100, `2` for 1000. Present only when
  `CAP.MDIO = 1`; reads 0 and is ignored otherwise.

Remaining bits reserved (read 0). Every bit but `SPEED` resets to `0`,
so a device comes out of reset neither receiving nor sending.

`SPEED` exists because a device cannot always tell what its PHY
negotiated: on some PHY interfaces neither the clock nor the data
says. The consumer reads the result from the PHY in any case, and
passes it on here. It writes only a speed set in `CAP.SPEEDS`. The
field resets to a speed of the device's choosing, and a change takes
effect at once: a frame in transit in either direction at that moment
may be lost.

While `RX_EN` is clear the device receives nothing and counts nothing
in [`RX_DROP`](#rx_drop-0x04c). A frame arriving as the bit is cleared
may still be stored. Clearing `TX_EN` is described under
[Transmit](#transmit).

### LINK (0x010)

- Bit [0]: `UP` — the link is established.
- Bits [9:8]: `SPEED` — the link's speed, encoded as `CTRL.SPEED`.
  Meaningful while `UP` is set.

Remaining bits reserved (read 0). Present only when `CAP.MDIO = 0`;
reads 0 otherwise, where the PHY is the place to ask.

### MDIO (0x014)

- Bits [15:0]: `DATA` — the value to write, or the value read.
- Bits [20:16]: `REG` — the PHY register.
- Bits [28:24]: `PHY` — the PHY's address on the management bus.
- Bit [30]: `WRITE` — the transaction writes `DATA`; when clear, it
  reads into `DATA`.
- Bit [31]: `BUSY` — a transaction is in progress. Read-only.

Remaining bits reserved (read 0). Present only when `CAP.MDIO = 1`;
reads 0 and ignores writes otherwise.

Writing the register while `BUSY` is clear starts one management
transaction (IEEE 802.3 clause 22) and sets `BUSY`. `BUSY` clears when
the transaction ends, within 1 ms; after a read, `DATA` then holds the
PHY's answer, and the other fields read as written. A write while
`BUSY` is set is ignored. A read from an address where no PHY answers
returns `0xFFFF`, which is how a consumer finds the PHY.

The register carries a whole transaction rather than the two
management pins. A consumer's interface to a PHY is read and write of
a register, and a device can turn that into a serial frame far more
cheaply than it could recover a transaction from pins driven one edge
at a time.

### ADDR_LO (0x018) / ADDR_HI (0x01C)

- `ADDR_LO` bits [31:0]: bytes 0–3 of the station address, byte 0 (the
  first on the wire) in bits [7:0].
- `ADDR_HI` bits [15:0]: bytes 4–5, byte 4 in bits [7:0].

Both reset to `0`. The address `02:11:22:33:44:55` is
`ADDR_LO = 0x33221102`, `ADDR_HI = 0x00005544`.

The device uses the address only to filter received frames (see
[Receive](#receive)). It does not write it into frames it sends: the
source address is part of the frame software supplies.

### FACTORY_LO (0x020) / FACTORY_HI (0x024)

- `FACTORY_LO` bits [31:0]: bytes 0–3, laid out as `ADDR_LO`.
- `FACTORY_HI` bits [15:0]: bytes 4–5, laid out as `ADDR_HI`.
- `FACTORY_HI` bit [31]: `VALID` — the device holds the address.

Present only when `CAP.ADDR = 1`; both read 0 otherwise.

A device may have to fetch its address after reset, from a memory on
a control bus of its own, say, and the fetch can fail. `VALID` sets
when the address is held and then stays set. A device sets it within
200 ms of reset or not at all, so a consumer that finds it clear after
that supplies an address of its own, as it does on a device without
`CAP.ADDR`.

The factory address is separate from `ADDR` so that it survives a
consumer choosing a different one: the device never copies it, and
`ADDR` holds only what software wrote.

### RX_GEOM (0x040) / TX_GEOM (0x050)

- Bits [15:0]: `SLOTS` — the number of slots in the ring, a power of
  two and at least 1.
- Bits [31:16]: `SLOT_SIZE` — the size of each slot in bytes, a power
  of two and at least 2048.

The two rings need not match. A device with memory to spare gives the
receive ring more slots, since frames arrive at the wire's pace and
leave at the consumer's.

### RX_PROD (0x044) / RX_CONS (0x048)

- `RX_PROD` bits [15:0]: frames the device has stored, counted since
  reset. Read-only.
- `RX_CONS` bits [15:0]: frames software has released, written by
  software.

Both reset to `0`. See [Slots](#slots) for how the counts name slots
and [Receive](#receive) for the protocol.

### RX_DROP (0x04C)

- Bits [31:0]: frames discarded because the receive ring was full,
  counted since reset. Wraps to 0.

Frames arrive at the wire's pace whether or not the consumer keeps up,
so a full ring is an ordinary condition for this device rather than a
fault. The count is what lets a consumer tell frames it was too slow
for from frames that never arrived.

### TX_PROD (0x054) / TX_CONS (0x058)

- `TX_PROD` bits [15:0]: frames software has queued, written by
  software.
- `TX_CONS` bits [15:0]: frames the device has finished with, counted
  since reset. Read-only.

Both reset to `0`. See [Slots](#slots) and [Transmit](#transmit).

### TX_WM (0x05C)

- Bits [15:0]: a threshold on the frames queued for transmission.

The `TX` interrupt source is asserted while the transmit ring holds
fewer frames than this, that is, while `TX_PROD - TX_CONS` is less
than `TX_WM`. Resets to `0`. No count is less than 0, so the source is
off until a consumer writes a threshold, and writing 0 turns it off
again.

Frames are copied into slots, so a consumer has nothing to release
when one has been sent: a finished frame matters only to a consumer
that was waiting for room. Such a consumer leaves the threshold at 0
while it has room. On finding the ring full it writes `SLOTS`, to be
woken as soon as one slot is free, or `SLOTS - k + 1`, to be woken
when *k* are. It writes 0 again when woken, which is also how it
declines to queue anything more.

A threshold above `SLOTS` holds the source asserted whatever the ring
holds.

## Slots

A ring is `SLOTS` slots of `SLOT_SIZE` bytes, laid end to end from the
start of its aperture: slot *s* of the receive ring begins at
`0x10000 + s * SLOT_SIZE`, and slot *s* of the transmit ring at
`0x8000 + s * SLOT_SIZE`. The transmit ring fits below the receive
aperture (`SLOTS * SLOT_SIZE` is at most `0x8000`), and the device's
`CFG_SIZE` window reaches the end of the receive ring.

Each slot holds one frame:

| Bytes     | Content                                                   |
|-----------|-----------------------------------------------------------|
| 0–1       | `LEN`: the frame's length in bytes, little-endian         |
| 2 onward  | The frame, from the destination address to the last byte of payload |

The frame carries no preamble and no frame check sequence; the device
adds both when sending and removes both when receiving. `LEN` counts
neither, nor itself.

The frame starts at byte 2 so that what follows the 14-byte Ethernet
header starts on a word boundary. Misaligned word accesses trap (see
[architecture.md](../architecture.md)), and the payload is usually a
header that software reads by the word, so a consumer's own buffer
keeps the frame 2 bytes past a word boundary as well. The slot then
copies into that buffer as whole words from its first word, both
sides word-aligned: `LEN` lands in the buffer's two spare bytes, and
the frame where the consumer wants it.

**Indices.** `RX_PROD`, `RX_CONS`, `TX_PROD` and `TX_CONS` are 16-bit
counts of frames that wrap from 65535 to 0. The frame a count refers
to lives in slot `count mod SLOTS`. Because `SLOTS` is a power of two,
that is the count's low bits, and the wrap at 65536 needs no handling.
A ring holds `(PROD - CONS) mod 65536` frames, from 0 to `SLOTS`, so
every slot is usable and a ring of one slot is legal.

Each count has one writer, and the two counts of a ring divide its
slots between the device and software:

| Counts                         | Receive ring                         | Transmit ring                    |
|--------------------------------|--------------------------------------|----------------------------------|
| From `CONS` up to `PROD`       | Stored frames, software's to read    | Queued frames, the device's to send |
| From `PROD` up to `CONS + SLOTS` | Free slots, the device's to fill   | Free slots, software's to fill   |

**Access.** Both apertures follow the bus's default access contract
(see [Access Width](../../hardware/bus-protocol.md#access-width)).
Reads of the receive aperture return the stored bytes at every width,
and writes to it are ignored. Writes to the transmit aperture commit
exactly the enabled byte lanes, so a consumer assembling a frame from
pieces of any length and alignment stores each piece as it lies; reads
of it return 0. Slot contents are undefined after reset.

A receive slot is stable from the moment `RX_PROD` counts it until
`RX_CONS` passes it, and reads of a slot outside that span return
undefined values. The device reads a transmit slot only between
`TX_PROD` counting it and `TX_CONS` passing it, and software writes a
slot only outside that span.

## Receive

While `CTRL.RX_EN` is set, the device stores each frame that passes
three tests:

- **It is well formed.** Its frame check sequence is correct and its
  length, without the check sequence, is from 60 to 1518 bytes.
- **It is addressed to this station.** Its destination is the address
  in `ADDR`, or the broadcast address, or a group address while
  `CTRL.ALLMULTI` is set. `CTRL.PROMISC` accepts every destination.
- **A slot is free.** The receive ring holds fewer than `SLOTS`
  frames.

A frame that passes is written to slot `RX_PROD mod SLOTS` with its
length, and `RX_PROD` then advances by one. Software never sees a
frame in progress.

A frame that fails the first two tests is discarded without trace,
`PROMISC` or not. One that fails only the third is discarded and
counted in [`RX_DROP`](#rx_drop-0x04c).

Software reads the frames from `RX_CONS` up to `RX_PROD` and releases
them by writing the count it has reached to `RX_CONS`, one frame at a
time or several at once. It writes only values from the current
`RX_CONS` up to `RX_PROD`. Releasing without reading is how a frame is
dropped.

## Transmit

Software queues a frame by writing it, and then its `LEN`, to slot
`TX_PROD mod SLOTS`, and then writing `TX_PROD + 1` to `TX_PROD`. It
may fill several free slots and advance `TX_PROD` past all of them in
one write, by at most the number of free slots.

While `CTRL.TX_EN` is set, the device sends the queued frames in
order. For each it sends the preamble, `LEN` bytes from the slot, zero
bytes up to a length of 60 where the frame is shorter, and the frame
check sequence. `TX_CONS` advances by one when the frame has left the
wire. A slot whose `LEN` is 0 or does not fit the slot sends nothing
and is passed over in the same way.

The device sends whatever the state of the link: a frame sent into a
link that is down is lost, and `TX_CONS` advances regardless, so the
ring never stalls on the link.

Clearing `TX_EN` discards the queued frames the device has not begun:
once the frame on the wire, if any, has ended, `TX_CONS` advances to
`TX_PROD`. The frames were copied into the slots, so a consumer holds
nothing that needs releasing, and a ring emptied at stop is one that
cannot send stale frames at the next start. A frame queued before
that moment is discarded with the rest, so a consumer that stops and
starts again waits for `TX_CONS` to reach `TX_PROD` before it queues
anything.

## Link Management

The link takes one of two forms, and `CAP.MDIO` says which.

**The device manages its own link** (`CAP.MDIO = 0`). It brings the
link up without software, runs at the speed the link settles on, and
reports the result in [`LINK`](#link-0x010). The consumer has nothing
to do but read it.

**Software manages a PHY** (`CAP.MDIO = 1`). The PHY negotiates the
link, and the consumer reads the outcome from the PHY's registers
through [`MDIO`](#mdio-0x014) and writes the speed to `CTRL.SPEED`.
Until it does, the device may be running at a speed other than the
link's, and frames do not get through.

Neither form raises an interrupt when the link changes. A consumer
that follows the link reads `LINK`, or the PHY's status, at an
interval of its own choosing; once a second is customary.

The device is full duplex: it sends without waiting for the medium to
be idle and detects no collisions. A consumer managing a PHY has it
advertise only full-duplex modes, at speeds set in `CAP.SPEEDS`.

## Driver Flow

### Bring-up — the class minimum

1. Read `RX_GEOM` and `TX_GEOM`.
2. Write the station address to `ADDR_LO` and `ADDR_HI`.
3. Write the value of `RX_PROD` to `RX_CONS`, releasing anything
   stored before.
4. Set `CTRL.RX_EN` and `CTRL.TX_EN`.

This serves any device that manages its own link. On one with
`CAP.MDIO`, the link steps under [A managed PHY](#a-managed-phy) come
first.

### Transmit

1. Compare `TX_PROD - TX_CONS` with `SLOTS`. If the ring is full,
   write `SLOTS` to `TX_WM` and wait for `TX`.
2. Write the frame to the slot from byte 2, then `LEN` to bytes 0–1.
3. Write `TX_PROD + 1` to `TX_PROD`.

### Receive

1. Read `RX_PROD`.
2. For each count from `RX_CONS` up to it, read the slot's first word
   for `LEN`, then copy `LEN + 2` bytes, rounded up to whole words,
   from the start of the slot to a word-aligned buffer. The frame
   begins 2 bytes into that buffer.
3. Write the count reached to `RX_CONS`.
4. Read `RX_PROD` again and repeat until it equals `RX_CONS`.

### Interrupts

1. Set `RX` and `TX` in `IRQ_ENABLE`. `TX` stays quiet while `TX_WM`
   is 0, so it costs nothing to leave enabled.
2. On interrupt, read `IRQ_STATUS`. If neither bit is set, the
   interrupt is another device's.
3. For `RX`, run the receive flow until the ring is empty. Releasing
   the last frame is what ends the condition.
4. For `TX`, write 0 to `TX_WM`, then queue what was waiting.

A handler that defers the receive work clears `RX` in `IRQ_ENABLE`
before it returns, and sets it again once the ring is empty.

### A managed PHY

1. Read PHY register 2 at each address from 1 to 31 through `MDIO`;
   an address that does not answer `0xFFFF` is a PHY's. Try address 0
   only when none of them answers: a PHY may answer there as well as
   at its own address, taking 0 for a call to every PHY, and it is
   one PHY all the same.
2. Have the PHY advertise the full-duplex modes at the speeds in
   `CAP.SPEEDS`, and no others.
3. Read the PHY's status at an interval. Whenever it reports a new
   link, write the speed it settled on to `CTRL.SPEED`.

### Station address

1. If `CAP.ADDR` is set, read `FACTORY_HI` until `VALID` sets, for at
   most 200 ms after reset.
2. With `VALID` set, use the factory address. Otherwise choose a
   locally administered one.
3. Write the address chosen to `ADDR_LO` and `ADDR_HI`.

## Scope of the Minimum

The minimum protocol covers full-duplex Ethernet at the speeds a
device reports, frames of up to 1518 bytes without the check sequence
(room for one VLAN tag, which passes as part of the frame), and
filtering on one station address, the broadcast address and,
wholesale, every group address.

Half duplex, pause frames, filtering on individual group addresses,
VLAN tag handling, checksum computation, longer frames, counters
beyond `RX_DROP`, and the newer management frame format (clause 45),
where present, are richer features behind `CFG_ID`.

## Relationship to Host Software

The interface is defined so that generic firmware can send and receive
frames with these registers alone, and so that one class-bound driver
serves every conformant device, sizing its work from `RX_GEOM` and
`TX_GEOM`. Under NetBSD that driver is a network interface that
copies frames between `mbuf` chains and slots; on a device with
`CAP.MDIO` it attaches `mii(4)`, whose PHY drivers manage the link
through `MDIO` and whose status-change callback supplies the value for
`CTRL.SPEED`.

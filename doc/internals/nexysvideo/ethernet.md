# Nexys Video: Ethernet

How the board realizes the Ethernet adapter
([`doc/internals/ethernet.md`](../ethernet.md)): its PHY, the adapter
that presents it at the device's seam, the clocks, the PHY's reset,
and where the station address comes from.

## The PHY

The board carries a Realtek RTL8211E-VL, a 10/100/1000 PHY, on RGMII
for data and MDIO for management, with a 25 MHz oscillator of its own
(reference manual, "Ethernet PHY"). The RGMII and MDIO pins are in
bank 13 at 2.5 V; the PHY's reset is in bank 34 at 3.3 V. Its
interrupt and power-management outputs reach the FPGA as well, and the
machine uses neither.

Its configuration pins set it up at power-on and at every reset:

- **Negotiation on, every speed advertised.** The PHY brings a link up
  by itself, whether or not the FPGA is configured.
- **Management address 1.**
- **No delay on either clock.** The PHY changes its outputs within
  half a nanosecond of the edges of the receive clock it sends, and
  samples its inputs on the edges of the transmit clock it is sent, so
  that clock has to arrive behind the data.

Several of those configuration pins are the receive data and control
pins, which the PHY reads as it leaves reset. The FPGA only listens on
them, and must put no pull resistor on them, so that what the board's
own resistors set is what the PHY reads.

## The Adapter

`eth_phy_rgmii_xc7` presents the PHY at the device's seam, on the
Artix-7's I/O cells, at 10 and 100 Mbit/s. Those are the speeds the
device reports, and software, which manages this PHY, keeps it from
negotiating 1000.

At 1000 Mbit/s RGMII moves a nibble on each clock edge, 4 ns apart,
and each clock has to reach its receiver about 2 ns behind the data.
With the PHY adding no delay, both delays would be the FPGA's to
supply: an input delay cell on the receive clock, calibrated against a
200 MHz reference, and a 125 MHz transmit clock with a copy a quarter
cycle late. The machine has no use for what those would buy, since
copying a frame takes the CPU longer than a gigabit link takes to
deliver one. At 10 and 100 Mbit/s a nibble lasts a whole clock cycle
of 400 or 40 ns, and the adapter needs none of them. Carrying 1000
is a change to this adapter alone: the device above the seam is the
same at every speed.

**Receive.** The receive clock, 25 or 2.5 MHz, comes in on a
clock-capable pin and drives the receive domain through a global
buffer. The data and control pins are captured in their input cells on
both of its edges: the rising edge takes the nibble and data valid,
and the falling edge the control pin's second half, which differs from
the first when the PHY reports an error.

Data valid marks the nibbles of a packet: a preamble, the start
delimiter and then the frame. A PHY may hand over any number of the
preamble's nibbles, odd or even, so nibbles paired from where data
valid rises can put the two halves of every byte in different pairs.
The adapter presents nothing of a preamble, and asks nothing of one:
whatever a packet holds ahead of its delimiter is passed over. It
waits for the two nibbles of the start delimiter, presents that byte,
and pairs nibbles from there, a byte on every other cycle. A nibble
left over when data valid falls is dropped. Between packets it
presents a byte time with data valid clear on every cycle, which is
how the device learns that a frame has ended.

The PHY changes those pins on the clock's own edges, so the capture
relies on the clock reaching the input cells after the data has. The
clock's path through its buffer is the longer of the two, and the room
is wide: a clock arriving anywhere in the half cycle after an edge,
20 ns at 100 Mbit/s, takes the right value. The constraints state the
PHY's outputs as changing on the clock's edges, so that the tools
check the capture and do not assume it, and the data pins take no
delay of their own on the way in.

**Transmit.** The transmit domain is the board's 100 MHz oscillator,
four of its cycles to one of the 25 MHz clock the PHY is sent at
100 Mbit/s and forty to one of the 2.5 MHz clock at 10. The adapter
makes that clock on its pin as a pattern, half its cycles high and
half low, from a register in the pin's output cell as the data and
control pins have. It takes a byte from the transmitter every 8 or 80
cycles and holds each nibble for 4 or 40.

The data and control pins change a quarter of the clock pin's period
before it rises. The PHY reads the control pin twice in a nibble: on
the clock's rise for transmit enable, which marks a packet's nibbles
as data valid does, and on the fall after it for an error, which it
finds when the two readings differ. So a nibble has to span a rise and
the fall behind it, in that order. Spanning a fall and then a rise,
the first and last nibbles of every packet would each read as an
error. A quarter period, 10 ns at 100 Mbit/s, then lies between every
change and both of the edges the PHY samples on, which stands in for
the delay the PHY does not add.

Changing speed switches no clock: the device's `i_speed` changes how
the adapter counts, and nothing else.

## Clocks

| Clock | Rate | From | Drives |
|-------|------|------|--------|
| Bus | the CPU's | The machine's clocking | Registers, MDIO |
| Receive | 25 or 2.5 MHz | The PHY | The receiver |
| Transmit | 100 MHz | The board's oscillator | The transmitter, the transmit pins |

The oscillator and the PHY's are unrelated. RGMII asks nothing more
of the transmit clock than its own accuracy, since the PHY re-times
what it is sent.

## Reset

The PHY is reset with the machine. Its registers are software's to
write, and only the reset pin puts all of them back: the reset bit in
its control register restores that register and the status register
and no other (datasheet, "BMCR"). Without the pin a driver would
inherit what the last one wrote, the selected register page and the
receive clock's output enable among it. With it, a driver finds the
PHY as its configuration pins set it. The link drops at each reset
and negotiates again while the machine boots.

The pin goes low with the board's reset and stays low for no less
than 10 ms, the pulse the PHY asks for (datasheet, "PHY Reset"). The
adapter counts that for itself on the oscillator and does not rely on
how long the board's reset lasts. The PHY then answers no management
frame for a further 30 ms. The machine is held in reset until those
have passed, as it is until the memory has calibrated, so software
never meets a PHY that is not ready.

## Station Address

A 24AA025E48 EEPROM on the board's I2C bus holds a unique address in
its read-only part, at bus address 1010111b. The audio codec's setup
owns the I2C master from reset, for about 30 ms. A reader takes the
master over when the setup lets go, reads the six bytes, and presents
them to the device with a valid bit, which the device reports as its
factory address. That is well inside the 200 ms the interface gives a
device to produce one. If the read fails the valid bit stays clear,
and the driver makes an address up.

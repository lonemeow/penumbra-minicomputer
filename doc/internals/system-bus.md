# System Bus — FPGA Implementation

> **Applies to:** all generations · shared hardware.

How the machine tops realise the sync form of the
[Penumbra Bus](../hardware/bus-protocol.md#sync-bus-mapping): where
each device's decode lives, how the responses merge, and the handshake
cell the simple devices share.  What a master and a slave owe each
other is specified in the bus protocol; this document records how the
RTL meets it.

## Request side: decode

The master's address and strobes reach every device.  A fixed device
(RAM, boot ROM, UART) gets its select from a `bus_devsel` comparator
instantiated beside it in the machine top, which hands the device a
pre-qualified strobe (`mem_re & ram_sel`).  An autoconfigured device
decodes itself: `autoconfig_dev` compares against the base software
assigned it and exports its select.  Either way the select is the
device's own decode, never a map the master keeps.

## Response side: `bus_response_fabric`

Each machine top instantiates one `bus_response_fabric`
(`hw/rtl/soc/`) and gives every slave a slot — its select, its ack
and its read data — indexed by the `SLV_*` localparams declared beside
the instance.  The cell implements the protocol's
[fabric rules](../hardware/bus-protocol.md#the-response-fabric): the
merged ack is a plain OR of the slaves' acks, read data is gated by
each slave's own ack, and a watchdog acknowledges, with `bus_fault`,
any access nothing answers within `TIMEOUT_CYCLES` (256 by default).
The deadline clears the slowest legitimate access with wide margin —
an SDRAM access landing on a refresh and a row conflict takes a few
tens of cycles, and a classic peripheral behind an async bus about
1 µs — so it fires only when no device answers, absent or stuck.

The fabric needs no knowledge of which devices exist.  The selects
reach it only for a simulation assertion that at most one of them
claims any access — structural for fixed decodes, and a software-held
property for autoconfigured ones, whose bases are assigned at run
time.

## Extending the bus off-chip

A board that carries the bus onto its headers adds a sync→async edge
bridge as one more slot.  Its select is the complement of the on-chip
selects, so it forwards exactly what nothing on-chip claims, and the
last on-chip autoconfig device's `cfg_out` continues through it, so
devices on the headers enumerate after the on-chip ones.  The fabric's
watchdog covers the external bus as well; the board sets
`TIMEOUT_CYCLES` to cover its slowest external device.

## Device handshakes

- **Simple devices** — ones that resolve every access in one cycle —
  instantiate `bus_device_simple` (`hw/rtl/soc/`).  It acknowledges
  each access the cycle after it is presented, and gives the device a
  one-cycle `o_read` / `o_write` pulse in the access's first cycle.
  The device keys every side effect on those pulses, never on its
  `i_re` / `i_we`, which stay high through the ack cycle.  The cell
  also checks the master's side of the contract: it asserts on a
  strobe dropped before its ack.
- **Variable-latency devices** decide their own ack from their own
  sequencer, under the same
  [slave obligations](../hardware/bus-protocol.md#slave-obligations).

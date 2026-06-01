# Penumbra Bus — Protocol

## Overview

The Penumbra Bus is the protocol that connects the CPU to memory
and I/O devices.  It is one protocol expressed in two equivalent
forms:

- **Sync form** — clock-aligned levels (`re`/`busy`/`rdata`).  Spoken
  everywhere inside the FPGA — between the CPU memory port, the L2
  cache (when present), the bus device-select layer, and every
  memory-mapped device.
- **Async form** — self-timed 4-phase handshake (`req`/`ack`).  Spoken
  across the discrete-build pin boundary.  Currently exists only as
  a specification; the FPGA build never uses it.

The two forms have identical causal semantics; only the signaling
differs.  RTL modules are written against the sync form (that's what
FPGA simulation and synthesis consume); the future discrete-build
form factor drives the async form from the same modules via a thin
sync-to-async wrapper at the pin boundary.

This document specifies the async form first (the original
signal-level specification), then maps it to the sync form in the
[Sync-Bus Mapping](#sync-bus-mapping) section.

## Terminology

These terms are used consistently throughout Penumbra's docs and
RTL.  Other docs reference them by this canonical naming.

| Term | Definition |
|---|---|
| **Penumbra Bus** | The protocol — both forms. |
| **Sync form** | Clock-aligned implementation; used everywhere inside the FPGA. |
| **Async form** | Self-timed 4-phase implementation; future discrete-build form, no live instance today. |
| **CPU memory port** | The `cpu_core` boundary (`o_mem_*` / `i_mem_*`).  The published interface between the CPU and whatever lives below it (L2, system bus).  Always sync form. |
| **System bus** | The shared interconnect outside `cpu_core` — hosts L2 (when present), the bus device-select layer (`bus_devsel`), and all memory-mapped devices.  Always sync form. |
| **External bus** | The portion of the system bus that crosses the FPGA pin boundary.  Currently doesn't physically exist; this is the future discrete-build target where the async form is used. |
| **Cache front-side** | A cache's upstream (CPU-facing) port. |
| **Cache back-side** | A cache's downstream (memory-facing) port. |

*Note:* "front" and "back" are from each cache's perspective, not
the CPU's.  L1's front-side talks to the core; L1's back-side talks
to the arbiter (which merges the I- and D-cache back-sides into the
CPU memory port).  L2's front-side talks to the CPU memory port;
L2's back-side talks to the system bus.

## Bus Signals (async form)

These are the signal names of the async / 4-phase form documented in
this section.  The sync form uses different names for several of
them (notably `req`/`ack` → `re`/`busy`, `bus_error` → `bus_fault`)
plus an additional `req_accepted` handshake pulse; the
[Sync-Bus Mapping](#sync-bus-mapping) section below has the
correspondence table.

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `addr[31:0]` | 32 | Master → Slave | Physical address |
| `data[31:0]` | 32 | Bidirectional | Read/write data |
| `we` | 1 | Master → Slave | Write enable |
| `byte_en[3:0]` | 4 | Master → Slave | Byte lane enables |
| `data_dir` | 1 | Master → all | Tri-state turnaround signal: 0=master drives data, 1=slave drives.  Async-form only — the sync form's separate `wdata`/`rdata` ports are inherently directional and need no equivalent. |
| `req` | 1 | Master → Slave | Request active |
| `ack` | 1 | Slave → Master | Acknowledge / completion |
| `bus_error` | 1 | Slave → Master | Access fault (sync-form name: `bus_fault`) |
| `rst` | 1 | Master → Slave | Bus reset |

## Four-Phase Handshake

### Read Cycle
1. Master asserts `req`, drives `addr`, `we=0`, `data_dir=1`.
2. Slave drives `data`, asserts `ack`.
3. Master deasserts `req` (captures data).
4. Slave deasserts `ack`.

### Write Cycle
1. Master asserts `req`, drives `addr`, `data`, `we=1`, `byte_en`, `data_dir=0`.
2. Slave commits write, asserts `ack`.
3. Master deasserts `req`.
4. Slave deasserts `ack`.

## Burst Transfers (Cache Line Fill)
The master performs back-to-back transfers by keeping `req` asserted and changing `addr` after each `ack`.

```
req:       ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___
addr:      ---[A+0 ][A+4 ][A+8 ][A+C ]-------
ack:       ______/‾‾\/‾‾‾\/‾‾‾\/‾‾‾\________
data:      ------[W0 ][W1  ][W2  ][W3  ]-----
```
The slave responds to each address independently.

## Access Width

The bus protocol permits sub-word access. A master may drive any
combination of `byte_en[3:0]` together with a byte-granular `addr`,
and the slave is expected to act on the indicated byte lanes.

### Default contract

Unless the device's own documentation says otherwise, the following
apply:

- **Writes.** The slave commits only the byte lanes whose `byte_en`
  bit is set. Lanes with `byte_en=0` are left unchanged.
- **Reads.** `byte_en` is advisory; the slave drives all 32 bits of
  `rdata` with each byte positioned at its natural lane (the byte at
  byte address `A+N` lives in `rdata[8N+7:8N]`, `N∈{0..3}`). The
  master extracts the requested lane(s) using `addr[1:0]` and the
  access size — the slave is not required to repack data based on
  `addr[1:0]`.

This is the contract that `simple_mem`, `fpga_ram`, the boot ROM and
the SDRAM stack implement. It is also the contract any general-
purpose memory-like slave should implement.

### Device-specific restrictions

An individual device's documentation MAY narrow the contract. The
most common restriction in Penumbra today is *word-strided access
only* — the device's registers occupy word slots in the address
map and the device handles only 32-bit transactions. The current
UART, SPI, ESP32 NIC and autoconfig config-space window are all of
this kind; see the per-device docs and the autoconfig section of
[bus.md](../system/bus.md) for the specifics.

When a device documents a restriction, software is responsible for
following it. The bus itself does not enforce per-device access
rules — adding runtime checks in every slave is exactly the cost
this layered contract is designed to avoid.

Violating a device restriction such as doing a full word read across
multiple sub-word MMIO registers or a sub-word partial read of a larger
device register results in undefined behavior and must not be relied
upon when implementing drivers. Devices are allowed to protect against
such misuse and assert `bus_fault` but that behavior is not required.

### Examples

- `LDW R1, [ram_addr]` against any RAM slave: returns the full word
  starting at `ram_addr`.
- `LDB R1, [ram_addr + 2]` against any RAM slave: slave drives the
  full word at `ram_addr & ~3`; the CPU's `byte_ext` selects
  `rdata[23:16]`. Correct under the default contract.
- `LDW R1, [uart_base + 4]` (read of UART register 1): supported,
  per the UART's word-strided contract.
- `LDB R1, [uart_base + 5]` (sub-word read of a word-strided
  register): not supported by the UART; result is governed by the
  rule defined in the section above.

## Sync-Bus Mapping

The sync form expresses the same protocol with clock-aligned levels
instead of self-timed edges.  Causal semantics are identical to the
async form above; the table below maps signals between the two
forms.

| Async signal | Sync signal | Direction | Notes |
|--------------|-------------|-----------|-------|
| `req` | `re` (read) / `we` (write) | master → slave | Held high while the transaction is in flight |
| `ack` rising | `busy` falling | slave → master | Slave drops `busy` on the cycle `rdata` is valid |
| `addr`, `data`, `byte_en` | `addr`, `wdata` (master→slave), `rdata` (slave→master), `byte_en` | — | Driven while `re`/`we` is held; sync form's separate wdata/rdata ports remove the need for `data_dir` |
| `bus_error` | `bus_fault` | slave → master | Same semantics |
| *(none)* | `req_accepted` | slave (arbiter) → master (cache) | Sync-form-only handshake pulse; see [Back-to-back bursts](#back-to-back-bursts-req_accepted) below |

### Sync read handshake

```
              cycle: 0   1   2   3   4
re:                 ___/‾‾‾‾‾‾‾‾‾‾‾‾‾\___
addr:               ---[A           ]----
busy:               ___/‾‾‾‾‾‾‾\_________
rdata:              ----------------[D]--
```

1. Master raises `re`, drives `addr` in cycle 0.
2. Slave raises `busy` (combinational or registered) and processes.
3. On the cycle `busy` drops, `rdata` is valid; master latches it.
4. Master drops `re` on the next cycle (or holds it with a new `addr`
   for a burst — see below).

### Master-side obligation

**The master must hold `re`/`we` (and `addr`/`data`/`byte_en`)
stable until the busy-drop cycle.**  This is the sync analogue of
"master holds `req` until `ack`" in the async form.  Slaves may
have different internal latching strategies (latch-immediately
like `simple_mem`, latch-after-pipeline like `sdram_bus_adapter`)
and the contract is unified: hold `re` until completion, and any
contract-compliant slave will work.

A master that pulses `re` for one cycle and drops it has no way to
know whether the slave accepted the access — strict slaves may
discard the request when they observe `re=0` in their processing
state, just as an async slave would discard a transaction where
`req` falls before `ack` rises.

### Slave-side obligation (registered-read devices)

**A slave with registered read output must assert `busy` for at
least one cycle on reads.**  The CPU's STALL sequencer latches
`rdata` on the cycle `busy` drops; a slave that reports `busy=0`
combinationally on the same cycle as `re` asserts will return
stale or zero data because its registered read flop hasn't yet
captured the requested word.

Slaves with truly combinational read paths (single-cycle BRAM,
constant ROM regions) can leave `busy=0` permanently — the data
they drive on the same cycle as `re` is the correct response.
Slaves with any registered stage on the read path (sysreg-style
devices with a 1-cycle access pipeline, FIFO-backed peripherals,
SDRAM controllers) must follow the `access_pending` pattern from
`hw/rtl/sim/sim_uart.sv`:

```
o_busy = i_re && !access_pending;
```

— assert `busy` on the same cycle as `re`, then drop it on the
cycle the registered read data is actually valid.  A device that
gets this wrong is invisible in lint and most testbenches; it
shows up as intermittent stale-data bugs that depend on whether
the master is currently bursting or single-stepping.  An early
`sim_spi.sv` bug had exactly this shape.

### Sync burst

```
              cycle: 0   1   2   3   4   5   6
re:                 ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\__
addr:               ---[A+0      ][A+4      ]---
busy:               ___/‾‾\______/‾‾\___________
rdata:              ------[D0]-------[D4]-------
```

`re` held across word boundaries; `addr` advances on the cycle
after each busy-drop.  Same shape as the async burst.

### Back-to-back bursts (`req_accepted`)

The diagram above shows a gap of one or more cycles between the
busy-drop of word *N* and the next `re`-driven request for word
*N+1*.  That gap exists because a naive master can't tell *when*
the slave latched its request — and on multi-master fabric (the
CPU's split I/D L1 caches sharing a single external bus via the
`cpu_bus_arbiter`) the master needs to know that to time the next
address.

The sync-form arbiter exposes a combinational `o_*_req_accepted`
pulse to each master.  It fires for **exactly one cycle** —
specifically the cycle the arbiter latches that master's
in-flight request — and lets the master advance its burst pointer
on the very next cycle without waiting for `busy` to fall.  With
this handshake, the arbiter supports back-to-back `BUSY → BUSY`
transitions: as one access completes, the next address is already
queued, and the bus stays driven word-after-word with no dead
cycle between successive completions.

```
              cycle: 0   1   2   3   4   5   6   7
re:                 ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\__
addr:               ---[A+0  ][A+4  ][A+8  ][A+C]---
req_accepted:       ___/‾\____/‾\____/‾\____/‾\___
busy:               ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\__
rdata:              ------[D0  ][D4  ][D8  ][DC]----
```

Slaves on a single-master shared bus (i.e. without an arbiter in
front) don't need to generate `req_accepted` — the contract reads
as "the master may treat `busy` going low and a fresh `re`
together as equivalent to the arbiter pulse."  Caches that drive
the arbiter (`cache_vipt.sv`, by way of `cpu_bus_arbiter.sv`) use
`i_req_accepted` to advance their `req_idx`/`resp_idx` counters
during line fills.  See `hw/rtl/penumbra1/cpu_bus_arbiter.sv` for the
RTL.

## Reset Timing

The `rst` line resets **autoconfig state** (the `configured` and
`cfg_seen_low` flip-flops in each device). It does not initialize
device-specific hardware (SDRAM controllers, SD cards, …) — each
device driver performs its own initialization after autoconfig
assigns a base address.

Two pulse sources drive the same line:

| Source                | Minimum duration | Rationale |
|-----------------------|------------------|-----------|
| Hardware (power-on)   | **10 ms**        | Covers PLL lock, power rail stabilization, crystal oscillator startup. Board implementations must meet this regardless of clock frequency. |
| Software (`BUSCTL.RST`)| **100 µs**      | Propagation through the async external bus: worst case is 74xx gate delays + backplane trace delays + RC settling. 74HC async clear is <100 ns, but the spec allows for long backplanes and slow LS/ALS parts. |

**Board implementation.** The hardware reset counter width must satisfy
`2^N / f_clk >= 10 ms`. At the current ULX3S 25 MHz system clock,
`N=18` gives ~10.5 ms (just meets the minimum) and the board uses
`N=19` for ~21 ms of margin (see `ulx3s_top.sv:175-180`). Faster
clocks need wider counters.

**Software implementation.** The boot ROM delay loop between asserting
and deasserting `BUSCTL.RST` must hold the pulse for at least 100 µs.
The current ROM uses a calibrated iteration count based on the system
clock frequency (`BUS_RESET_DELAY_ITERS` in `boot_rom.c`).

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

## Bus Signals

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `addr[31:0]` | 32 | Master → Slave | Physical address |
| `data[31:0]` | 32 | Bidirectional | Read/write data |
| `we` | 1 | Master → Slave | Write enable |
| `byte_en[3:0]` | 4 | Master → Slave | Byte lane enables |
| `data_dir` | 1 | Master → all | 0=master drives data, 1=slave drives |
| `req` | 1 | Master → Slave | Request active |
| `ack` | 1 | Slave → Master | Acknowledge / completion |
| `bus_error` | 1 | Slave → Master | Access fault |
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

## Sync-Bus Mapping

The sync form expresses the same protocol with clock-aligned levels
instead of self-timed edges.  Causal semantics are identical to the
async form above; the table below maps signals between the two
forms.

| Async signal | Sync signal | Direction | Notes |
|--------------|-------------|-----------|-------|
| `req` | `re` (read) / `we` (write) | master → slave | Held high while the transaction is in flight |
| `ack` rising | `busy` falling | slave → master | Slave drops `busy` on the cycle `rdata` is valid |
| `addr`, `data`, `byte_en` | same names | — | Driven while `re`/`we` is held |
| `bus_error` | `bus_fault` | slave → master | Same semantics |

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
`2^N / f_clk >= 10 ms`. At 12.5 MHz, `N=17` gives 10.5 ms (the ULX3S
uses `N=18` for margin). Faster clocks need wider counters.

**Software implementation.** The boot ROM delay loop between asserting
and deasserting `BUSCTL.RST` must hold the pulse for at least 100 µs.
The current ROM uses a calibrated iteration count based on the system
clock frequency (`BUS_RESET_DELAY_ITERS` in `boot_rom.c`).

# Penumbra Bus — Hardware Protocol

## Overview

The Penumbra Bus is an **asynchronous, demultiplexed** bus with separate address and data lines. Transfers use a four-phase request/acknowledge handshake with no shared clock.

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

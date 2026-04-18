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

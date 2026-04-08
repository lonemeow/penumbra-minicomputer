# Penumbra System Bus - Overview

## Goals

- Connect the CPU core to memory and I/O devices
- Support DMA transfers (devices reading/writing memory without CPU involvement)
- Simple enough to understand and debug, but realistic enough to learn from
- Feasible to implement in discrete 74xx logic for a future physical build

## Bus Architecture

Penumbra uses a custom bus protocol (the **Penumbra Bus**) designed for simplicity, learnability, and discrete 74xx feasibility. The system has three bus domains:

1. **Internal synchronous bus** — inside the FPGA (or on the CPU board in discrete), connects the CPU core, caches, and MMU. Runs at the CPU clock rate.
2. **Penumbra Bus (external, asynchronous)** — connects the CPU complex to memory and I/O peripherals. Uses an asynchronous request/acknowledge handshake, no shared clock. Separate address and data buses (demultiplexed).
3. **System register bus (sideband)** — lightweight bus for `WRSYS`/`RDSYS` to CPU-adjacent devices. Shares the internal data bus.

The async external bus eliminates clock distribution problems and works identically in both the FPGA prototype and the future discrete build.

## Full System Block Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│                     CPU COMPLEX (synchronous domain)                      │
│                                                                          │
│  ┌────────────────────────────────────┐                                  │
│  │           CPU Core                  │                                  │
│  │  ┌──────────┐  ┌────────────────┐  │                                  │
│  │  │ Register  │  │ Microcode      │  │                                  │
│  │  │ File      │  │ Sequencer+ROM  │  │                                  │
│  │  │ R0-R13    │  │ (~48-bit wide) │  │                                  │
│  │  │ SP (USP/  │  └────────────────┘  │                                  │
│  │  │   SSP)    │          │           │   busy/done    ┌──────────┐     │
│  │  │ PC        │          v           │<─────────────> │ MUL unit │     │
│  │  │ SR        │  ┌────────────┐      │   busy/done    ├──────────┤     │
│  │  └──────────┘  │    ALU     │      │<─────────────> │ DIV unit │     │
│  │       │        └────────────┘      │   busy/done    ├──────────┤     │
│  │       v              │             │<─────────────> │ FPU      │     │
│  │   ┌──────────────────────┐         │                │ (future) │     │
│  │   │  Internal Data Bus   │         │                └──────────┘     │
│  │   └─────┬────────────────┘         │                                  │
│  └─────────┼──────────────────────────┘                                  │
│            │                                                              │
│    ┌───────┴───────┐  System Register Bus (sideband)                     │
│    │  data[31:0]   ├──────────────────────────────────────────┐          │
│    │  (shared)     │  sys_cycle, sys_dev[3:0],                │          │
│    │               │  sys_reg[3:0], sys_we                    │          │
│    └───────┬───────┘                                          │          │
│            │                 ┌────────┐ ┌───────┐ ┌────────┐  │          │
│    ┌───────┴───────┐        │D-cache │ │I-cache│ │  Bus   │  │          │
│    │  MMU / TLB    │◄───────┤ dev 2  │ │ dev 3 │ │ Ctrl  │  │          │
│    │  (dev 0)      │ sysreg └───┬────┘ └───┬───┘ │ dev 4 │  │          │
│    └───────┬───────┘            │          │     └───┬───┘  │          │
│            │                    │          │                  │          │
│   phys addr + C bit             │          │                  │          │
│            │                    │          │                  │          │
│    C=1? ───┼─── C=0?           │          │                  │          │
│     │      │      │             │          │                  │          │
│  ┌──v───┐  │  ┌───v──┐         │          │                  │          │
│  │I-$   │  │  │D-$   │         │          │                  │          │
│  │(RO)  │  │  │(WT)  │         │          │                  │          │
│  └──┬───┘  │  └──┬───┘         │          │                  │          │
│     └──┬───┘─────┘  bus bypass  │          │                  │          │
│        │       │    (uncached)  │          │                  │          │
│        v       v               │          │                  │          │
│  ┌─────────────────────┐       │          │                  │          │
│  │    Bus Arbiter      │◄──────┼──────────┘                  │          │
│  │ (CPU vs DMA master) │       │  DMA bus master             │          │
│  └─────────┬───────────┘       │                             │          │
│            │                   │                             │          │
│  ┌─────────┴───────────┐       │                             │          │
│  │    Async Bridge     │       │                             │          │
│  │ (sync ←→ async)     │       │                             │          │
│  └─────────┬───────────┘       │                             │          │
└────────────┼───────────────────┼─────────────────────────────┘          │
             │                   │                                         │
═════════════╪═══════════════════╪══ Penumbra Bus (async, demultiplexed) ══╪══
             │                   │                                         │
             │    addr[31:0], data[31:0], req, ack,                        │
             │    we, byte_en[3:0], data_dir, bus_error                    │
             │                   │                                         │
      Shared bus — each device decodes its own address range              │
             │     (no central address decoder)                                │
             │                                                                 │
      ── ────┼──────── cfg (daisy-chained autoconfig) ──────────────────┐     │
             │                                                          │     │
     ┌───────┼───────┬───────────┬───────────┬───────────┐              │     │
     │       │       │           │           │           │              │     │
     v       v       v           v           v           v              │     │
  ┌──────┐┌────┐┌──────┐┌─────┐┌─────┐┌───────┐┌─────────┐            │     │
  │System││Boot││ UART ││ SPI ││GPIO ││Wiznet ││ (future) │            │     │
  │RAM   ││ROM ││ [hw] ││[ac] ││[ac] ││ [ac]  ││  [ac]   │            │     │
  │ [hw] ││[hw]││      ││     ││     ││       ││         │            │     │
  └──────┘└────┘└──┬───┘└──┬──┘└──┬──┘└──┬────┘└─────────┘            │     │
       [hw]=hardwired  └───┬───┴──┬───┴──────┘                          │     │
       [ac]=autoconfigured v      v                                      │     │
            ┌──────────────────────┐     NMI ──────────────────────────┘     │
            │  Priority Encoder    │──── IRQ + vector[2:0] ──> CPU           │
            │  (74x148 in discrete)│                                         │
            └──────────────────────┘
```

## Penumbra Bus Protocol

### Overview

The Penumbra Bus is an **asynchronous, demultiplexed** bus with separate address and data lines. Transfers use a four-phase request/acknowledge handshake with no shared clock.

This design is motivated by two constraints:
- **Discrete 74xx feasibility:** Clock distribution across multiple boards is hard; async handshakes are self-timed and work at any speed and wire length
- **FPGA-to-external validation:** The async protocol lets the FPGA connect to external memory/peripheral boards for testing the discrete build's memory system

### Bus Signals

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `addr[31:0]` | 32 | Master → Slave | Physical address (separate from data) |
| `data[31:0]` | 32 | Bidirectional | Read/write data |
| `we` | 1 | Master → Slave | Write enable (read implied when `req AND NOT we`) |
| `byte_en[3:0]` | 4 | Master → Slave | Byte lane enables |
| `data_dir` | 1 | Master → all | Data bus direction (0=master drives, 1=slave drives) — controls 74x245 transceivers in discrete |
| `req` | 1 | Master → Slave | Request — master is presenting a valid transfer |
| `ack` | 1 | Slave → Master | Acknowledge — slave has completed the transfer |
| `bus_error` | 1 | Slave → Master | Access fault (unmapped address or timeout) |
| `rst` | 1 | Master → Slave | Bus reset — resets all devices to unconfigured state. Directly driven by system hardware reset; also software-triggerable via the bus controller sysreg. Active high pulse. |
| `cfg` | 1 | Daisy-chained | Autoconfig chain — see [Device Discovery](#device-discovery-autoconfig) |

**Total: 74 signals** (32 addr + 32 data + 10 control).

### Four-Phase Handshake

All transfers follow a four-phase handshake. No clock is required on the bus.

**Read cycle:**
```
req:       ___/‾‾‾‾‾‾‾‾‾\________
addr:      ---[ VALID ADDR ]-------
we:        ________________________  (low = read)
data_dir:  ___/‾‾‾‾‾‾‾‾‾\________  (slave drives data)
data:      ----------[VALID DATA]--  (slave drives)
ack:       ________/‾‾‾‾‾\________
                    ^
                    master samples data here

Phase 1: Master asserts req, drives addr, we=0, data_dir=1
Phase 2: Slave drives data, asserts ack
Phase 3: Master deasserts req (data captured)
Phase 4: Slave deasserts ack — bus idle
```

**Write cycle:**
```
req:       ___/‾‾‾‾‾‾‾‾‾\________
addr:      ---[ VALID ADDR ]-------
we:        ___/‾‾‾‾‾‾‾‾‾\________
data_dir:  ________________________  (low = master drives data)
data:      ---[ VALID DATA ]-------  (master drives)
byte_en:   ---[ VALID SEL  ]-------
ack:       ________/‾‾‾‾‾\________
                    ^
                    slave commits write here

Phase 1: Master asserts req, drives addr, data, we=1, byte_en, data_dir=0
Phase 2: Slave completes write, asserts ack
Phase 3: Master deasserts req
Phase 4: Slave deasserts ack — bus idle
```

### Protocol Rules

1. A transfer starts when `req` rises and ends when both `req` and `ack` have returned low
2. Master must hold `addr`, `data` (if write), `we`, `byte_en`, `data_dir` stable while `req` is asserted
3. Slave must hold `data` (if read) stable from when `ack` rises until `req` falls
4. `bus_error` is mutually exclusive with `ack` — either the transfer succeeds or it faults
5. `data_dir` follows `we` (0 on writes = master drives, 1 on reads = slave drives)
6. Arbiter does not preempt while `req` is asserted (burst-safe — see below)

### Burst Transfers

For cache line fills (4 words), the master performs back-to-back transfers by keeping `req` asserted and changing `addr` after each `ack`:

```
req:       ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\___
addr:      ---[A+0 ][A+4 ][A+8 ][A+C ]-------
ack:       ______/‾‾\/‾‾‾\/‾‾‾\/‾‾‾\________
data:      ------[W0 ][W1  ][W2  ][W3  ]-----
```

The master drives a new address after each `ack`. The slave responds to each address independently — no internal burst counter required. The arbiter sees `req` held high and does not preempt, ensuring the burst completes atomically.

Slave devices that can take advantage of sequential addresses internally (e.g., SDRAM controllers on FPGA) may detect the pattern and optimize, but this is invisible to the protocol.

### Bus Error / Timeout

When an access hits unmapped address space, no device responds — no `ack` is asserted. A **bus timeout counter** on the master side detects this: if `req` is held for N cycles of a local reference oscillator without `ack`, it asserts `bus_error`. The CPU treats this as a **bus fault exception** (vector 0, `VEC_BUS_FAULT`). The MMU latches FAULT_ADDR (virtual address) and FAULT_STATUS (type=`FAULT_BUS`, access info). This is how the OS probes for RAM size at boot (bypass mode) and optional devices (MMU-enabled, like NetBSD `bus_space_peek`).

In discrete, the timeout is a simple counter chip (e.g., 74x163 + comparator). On FPGA, it is a configurable down-counter in the async bridge. This catches both unmapped addresses and hung peripherals. There is no central address decoder or "default slave" — each device is responsible for recognizing its own address range (see [Address Decoding](#address-decoding)).

### Byte Lane Enables

The bus is 32 bits wide. `byte_en[3:0]` indicates which bytes are active:

| Access type | `byte_en` | `addr[1:0]` | Data bus lanes used |
|------------|-----------|-------------|---------------------|
| Word | `1111` | `00` | All 4 bytes |
| Halfword | `0011` | `00` | Bytes 0-1 (lower) |
| Halfword | `1100` | `10` | Bytes 2-3 (upper) |
| Byte | `0001` | `00` | Byte 0 |
| Byte | `0010` | `01` | Byte 1 |
| Byte | `0100` | `10` | Byte 2 |
| Byte | `1000` | `11` | Byte 3 |

**Alignment is enforced by the CPU** — word accesses must be 4-byte aligned, halfword accesses must be 2-byte aligned. Peripherals and memory controllers can assume all bus transfers are correctly aligned.

**Reads:** The slave always drives the full 32-bit `data[31:0]`. The CPU extracts and optionally sign-extends the relevant byte(s) internally.

**Writes:** Only the byte lanes indicated by `byte_en` are valid. Memory (SDRAM, SRAM) must support per-byte writes. In discrete, this naturally falls out of using byte-wide SRAMs (e.g., 4x AS6C4008 512K×8), each gated by one `byte_en` bit.

## Internal Synchronous Bus

Inside the CPU complex (FPGA fabric or discrete CPU board), a synchronous internal bus connects the caches and arbiter. This uses a standard `valid`/`ready` handshake clocked by the CPU clock:

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `addr[31:0]` | 32 | Master → Slave | Physical address |
| `data[31:0]` | 32 | Bidirectional | Read/write data |
| `we` | 1 | Master → Slave | Write enable |
| `re` | 1 | Master → Slave | Read enable |
| `byte_en[3:0]` | 4 | Master → Slave | Byte lane enables |
| `valid` | 1 | Master → Slave | Bus cycle active |
| `ready` | 1 | Slave → Master | Transfer complete this cycle |

Transfer completes on the rising clock edge where both `valid` and `ready` are high.

### Async Bridge

The **async bridge** sits between the internal synchronous bus and the external Penumbra Bus. It converts between the two domains:

- Accepts `valid`/`ready` requests from the internal bus
- Performs the four-phase async handshake on the Penumbra Bus
- Synchronizes the `ack` signal back to the CPU clock domain (double flip-flop synchronizer)
- Asserts `ready` on the internal bus when the external transfer completes

On the FPGA, the bridge connects to GPIO pins driving the external bus. In discrete, the bridge is implicit — the CPU board's bus interface directly drives the backplane.

## System Register Bus (Sideband)

Lightweight control bus for `WRSYS`/`RDSYS` privileged instructions to CPU-adjacent system devices.

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `data[31:0]` | 32 | Bidirectional | **Shared with internal data bus** — no additional data lines |
| `sys_cycle` | 1 | CPU → Devices | System register access in progress |
| `sys_dev[3:0]` | 4 | CPU → Devices | Target device select |
| `sys_reg[3:0]` | 4 | CPU → Devices | Register index within device |
| `sys_we` | 1 | CPU → Devices | Write enable (1 = WRSYS, 0 = RDSYS) |

New signals: **10 lines**. No arbitration needed — microcode guarantees these cycles never overlap with memory bus cycles. Each device has a 4-bit comparator on `sys_dev` (one 74x85 in discrete).

The system register bus is **internal only** — it does not cross the async bridge to the external Penumbra Bus. All devices on the sysreg bus (MMU, timer, DMA controller) are part of the CPU complex.

## CPU ↔ MMU Interface

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `virt_addr[31:0]` | 32 | CPU → MMU | Virtual address to translate |
| `access_type[1:0]` | 2 | CPU → MMU | Read / Write / Execute |
| `user_mode` | 1 | CPU → MMU | Current privilege (from SR S bit) |
| `phys_addr[31:0]` | 32 | MMU → CPU | Translated physical address |
| `cacheable` | 1 | MMU → CPU | C bit from page table entry |
| `hit` | 1 | MMU → CPU | TLB hit (translation valid) |
| `fault` | 1 | MMU → CPU | Access violation or page not present |

On `fault`, the MMU latches the faulting address and reason into its FAULT_ADDR and FAULT_STATUS registers (readable via sysreg bus).

## Interrupt Signals

The CPU has two interrupt inputs with separate vectors:

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `timer_irq` | 1 | Timer (sysreg) → CPU | Timer underflow (VEC_TIMER=1, higher priority) |
| `irq` | 1 | External bus → CPU | Shared wired-OR from all bus devices (VEC_EXT_IRQ=9) |

The **timer** is an internal sysreg device (device 7) with its own dedicated
vector. It does not use the external bus interrupt line.

The **external bus** carries a single `irq` wire (active-high, wired-OR).
All external devices (UART, Ethernet, SPI, etc.) share this line. The
interrupt handler reads the interrupt controller (future sysreg device 8) or
polls individual device status registers to determine the source. Software
handles priority in the handler.

This design minimises bus wiring (one signal vs. 8+ lines) and avoids
autoconfig complexity for interrupt assignment. Each external device can
report its interrupt status via its own MMIO registers.

## Long-Latency Unit Interface

Uniform interface between the CPU and each long-latency functional unit (multiplier, divider, future FPU).

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `operand_a[31:0]` | 32 | CPU → Unit | First operand |
| `operand_b[31:0]` | 32 | CPU → Unit | Second operand |
| `op[3:0]` | 4 | CPU → Unit | Operation select (for FPU: add/sub/mul/div/etc.) |
| `start` | 1 | CPU → Unit | Begin operation |
| `busy` | 1 | Unit → CPU | Operation in progress |
| `done` | 1 | Unit → CPU | Result ready (single-cycle pulse) |
| `result[31:0]` | 32 | Unit → CPU | Operation result |

The microcode sequencer holds the micro-PC when the selected unit's `busy` is asserted.

## DMA Handshake

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `dma_req` | 1 | Peripheral → DMA Ctrl | Peripheral requests a DMA transfer |
| `dma_ack` | 1 | DMA Ctrl → Peripheral | DMA controller acknowledges / transfer active |

DMA-capable peripherals (Wiznet at minimum) use dedicated request/acknowledge pairs. The DMA controller is configured via sysreg bus (source addr, dest addr, length, direction) and raises an interrupt on completion.

## Bus Topology

Shared-bus connecting via the Penumbra Bus. Each device decodes its own address range — the bus carries no decode logic.

**Hardwired devices** (fixed addresses, always present):
- CPU complex (bus master, via async bridge)
- DMA controller (bus master)
- System RAM (SDRAM on FPGA, SRAM in discrete — hardwired at 0x0, size probed)
- Boot ROM (64 KB at 0xFFFF_0000)
- Console UART (4 KB at 0xFF00_0000)

**Autoconfigured devices** (addresses assigned at boot via config chain):
- Expansion RAM (additional memory cards)
- SPI (SD card, other peripherals)
- GPIO
- Wiznet Ethernet adapter (MMIO + DMA)
- Future expansion cards

## Device Discovery (Autoconfig)

Devices on the Penumbra Bus support automatic discovery and address assignment at boot time. This eliminates hardcoded address maps for expansion devices and enables plug-and-play on the discrete backplane. The protocol is inspired by the Amiga Zorro II/III autoconfig mechanism.

### Hardwired vs Autoconfigured Devices

A small set of **hardwired devices** must be functional before any software runs — they have fixed, design-time base addresses and are always present:

- **System RAM** (`0x0000_0000`) — base-board memory. Size unknown at boot; the boot ROM probes upward by attempting reads until a bus fault occurs. Expansion RAM cards are separate devices that get autoconfigured addresses above system RAM.
- **Console UART** (`0xFF00_0000`) — serial console for boot diagnostics. Must be available to print debug output during the autoconfig process itself.
- **Boot ROM** (`0xFFFF_0000`) — contains the reset vector and autoconfig enumeration code.

All other devices are **autoconfigured**: they start in an unconfigured state after reset and receive their base addresses from the boot ROM's autoconfig routine.

### Bus Reset

The `rst` signal resets all autoconfigurable devices on the bus back to their unconfigured state. It is asserted in two situations:

1. **Hardware reset:** The FPGA (or discrete reset circuit) drives `rst` high during system power-on or hard reset. All devices, including the CPU, start from a known state.
2. **Software reset:** The boot ROM (or OS) writes to the bus controller sysreg to pulse `rst`. This resets external devices without resetting the CPU, allowing the software to re-run the autoconfig protocol (e.g., after hot-plug, or during an OS reboot that doesn't involve a hardware power cycle).

In discrete: `rst` is directly wired to each device's config flip-flop (active-high async clear). The software-triggered pulse comes from a flip-flop in the bus controller, OR'd with the hardware reset signal.

### The Config Chain

A single `cfg` signal is **daisy-chained** through all autoconfigurable devices on the bus:

```
                 cfg_in    cfg_out    cfg_in    cfg_out    cfg_in
Bus controller ───────> Device 0 ───────> Device 1 ───────> Device 2 ──> ...
  (sysreg)         (1st unconfigured    (2nd unconfigured
                    device responds)     device waits)
```

- After reset (`rst` pulse), all autoconfigurable devices return to **config state** (unconfigured).
- A device in config state **blocks** `cfg` — it does not pass `cfg_out` to the next device.
- A device in enabled state (already configured) **passes** `cfg` through, but only after `cfg_en` has been toggled (deasserted then reasserted) since the device was configured. This prevents the chain from propagating during the same bus cycle that configured the device, which would cause the next device to see a stale write. See [CFG_EN Toggle Protocol](#cfg_en-toggle-protocol) below.
- Only the **first unconfigured device** in the chain sees `cfg_in = 1` and responds to config cycles.

Discrete implementation: two flip-flops per device (`configured` and `cfg_seen_low`, both async-cleared by `rst`), one AND gate (`cfg_out = cfg_in & configured & cfg_seen_low`). `cfg_seen_low` is set when `cfg_en` goes low after configuration, preventing premature chain propagation.

### Bus Controller Sysreg Device

The bus controller is a CPU-internal sysreg device (`SYSDEV_BUS`, device 4) that controls the `rst` and `cfg` signals on the Penumbra Bus. It has a single register:

| Sysreg | Name | R/W | Description |
|--------|------|-----|-------------|
| 0 | `BUSCTL` | R/W | Bit 0: `RST` — assert/deassert bus reset (sticky). Bit 1: `CFG_EN` — enable config chain and config address decode on the bus. |

- **RST (bit 0):** A plain R/W bit. Writing 1 asserts `rst` on the bus, returning all autoconfigured devices to their unconfigured state. Writing 0 deasserts it. Software controls the timing: assert RST, delay as needed for slow devices on the async bus, then deassert. There is no hardware auto-clear — the external bus is asynchronous, so "one cycle" has no well-defined meaning for external devices.
- **CFG_EN (bit 1):** When set, the bus controller asserts `cfg` on the daisy chain and enables the config address range (`0xFE00_0000`). When clear, `cfg` is deasserted, and accesses to the config address range produce a bus fault (unmapped). Software must set `CFG_EN` before reading/writing config space, and clear it when enumeration is complete.

Reset default: `0x00` (config mode disabled, bus reset deasserted).

Discrete implementation: two flip-flops (RST, CFG_EN) driven by the sysreg write bus, one OR gate (hardware reset | software RST → bus `rst`).

### Config Space

When `CFG_EN` is set, a fixed address range at `0xFE00_0000` (32 bytes, 8 word-aligned registers) becomes active on the memory bus. Reads and writes to this range go to the **first unconfigured device** in the config chain (the one whose `cfg_in = 1`). The address lines carry the config register offset; the device's eventual base address is not involved.

| Address | R/W | Name | Description |
|---------|-----|------|-------------|
| `0xFE00_0000` | R | **CFG_CLASS** | Device class code (see table below) |
| `0xFE00_0004` | R | **CFG_SIZE** | Required address space in bytes (power-of-2) |
| `0xFE00_0008` | R | **CFG_ID** | Device ID (manufacturer + product, or 0 for generic) |
| `0xFE00_000C` | R | **CFG_NAME0** | Device name bytes 0–3 (packed LE, null-padded) |
| `0xFE00_0010` | R | **CFG_NAME1** | Device name bytes 4–7 |
| `0xFE00_0014` | R | **CFG_NAME2** | Device name bytes 8–11 |
| `0xFE00_0018` | R | **CFG_NAME3** | Device name bytes 12–15 |
| `0xFE00_001C` | W | **CFG_BASE** | Assigned base address. Writing transitions the device to enabled state: it latches the base address and begins responding to normal bus cycles at that address. The `cfg` chain does NOT propagate immediately — software must toggle `CFG_EN` (deassert then reassert) to advance to the next device. See [CFG_EN Toggle Protocol](#cfg_en-toggle-protocol). |

**Device class codes (CFG_CLASS):**

The class code identifies the **base register protocol** the device implements. Generic firmware (boot ROM, stage 1 bootloader) can use any device whose class it understands — no device-specific driver needed.

| Value | Name | Base protocol | Description |
|-------|------|---------------|-------------|
| 0 | `CLASS_UNKNOWN` | (none) | No standard register protocol. Needs a device-specific OS driver matched by `CFG_ID`. Cannot be used at boot without explicit support. |
| 1 | `CLASS_MEMORY` | (none — just address space) | Plain memory (RAM, ROM). No registers to program — once the autoconfig base address is assigned, software reads/writes it directly. Boot ROM maps it as additional RAM. |
| 2 | `CLASS_UART` | NS16450 register layout | UART-compatible serial port. Boot ROM can use it as a console (RBR/THR/LSR polling). NetBSD `com(4)` driver works unmodified. |
| 3 | `CLASS_SPI` | Penumbra SPI master (DATA/STATUS/CONTROL/CLKDIV) | SPI controller with the standard 4-register interface. Boot ROM knows how to do SD-SPI through this to load the stage 1 bootloader. |
| 4–255 | — | — | Reserved for future standard protocols |

**Extended devices:** A device with extra capabilities (e.g., an SPI controller with a built-in DMA engine) still reports the base class (`CLASS_SPI`) and implements the base register interface. The boot ROM uses only the base registers. Later, the OS driver reads `CFG_ID` to detect the specific variant and enables extended features. This ensures any `CLASS_SPI` device can be booted from, even if the OS hasn't loaded a device-specific driver yet.

**Device name (CFG_NAME0–3):** A 16-byte null-padded ASCII string, packed little-endian — same format as the CPU/machine name in the `sysid` sysreg. Examples: `"SPI"`, `"WizNet W5500"`, `"Exp. RAM 4MB"`. The boot ROM prints this during enumeration for diagnostic purposes. It makes autoconfig debugging much easier when you can see which physical card on the backplane corresponds to which probe step.

If no unconfigured device remains in the chain, reads to config space produce a **bus fault** (no device responds → bus timeout). Software uses this to detect the end of the device list.

Config space decode: one address comparator (same `bus_devsel` pattern as other devices) gated by `cfg`. In discrete: one 74x85 comparator + one AND gate with `cfg`.

### CFG_EN Toggle Protocol

**Software must deassert and reassert `CFG_EN` after configuring each device.** This is a hard requirement of the autoconfig protocol.

When `CFG_BASE` is written, the device latches its base address and transitions to the enabled state. However, the `cfg` chain does **not** propagate to the next device until `CFG_EN` has been toggled. This prevents a race condition where the next device in the chain sees the same write that configured the previous device.

The hardware enforces this with a `cfg_seen_low` flip-flop in each device: `cfg_out = cfg_in & configured & cfg_seen_low`. The `cfg_seen_low` flag is cleared on reset and set when `cfg_en` goes low after the device is configured. Until software toggles `CFG_EN`, the newly-configured device blocks the chain just like an unconfigured device.

**Software sequence per device:**
1. Read config registers (`CFG_CLASS`, `CFG_SIZE`, etc.) — `CFG_EN` is asserted
2. Write `CFG_BASE` with the assigned address — device configures
3. Clear `CFG_EN`: `WRSYS SYSDEV_BUS, BUSCTL, 0` — chain settles, `cfg_seen_low` sets
4. Set `CFG_EN`: `WRSYS SYSDEV_BUS, BUSCTL, CFG_EN` — next unconfigured device becomes active
5. Repeat from step 1 for the next device

**Why this matters:** On the async external bus, a write to `CFG_BASE` takes time (4-phase handshake). The CPU stalls until the write completes (bus busy). During this time, the chain could propagate and a subsequent device could see the write address still on the bus. The toggle ensures the chain only advances when software is ready.

**Discrete 74xx implementation:** One extra flip-flop per device (`cfg_seen_low`), set when `cfg_en` falls while `configured` is high.

### Autoconfig Boot Sequence

1. CPU boots from ROM at `0xFFFF_0000`, hardwired devices (system RAM, UART, ROM) already functional
2. Boot ROM probes system RAM size by reading upward from `0x0000_0000` until bus fault
3. Boot ROM asserts bus reset: `WRSYS SYSDEV_BUS, BUSCTL, 1` (RST=1)
4. Boot ROM delays (short loop — enough for slow async bus devices to see the reset)
5. Boot ROM deasserts reset and enables config: `WRSYS SYSDEV_BUS, BUSCTL, 2` (RST=0, CFG_EN=1)
6. Boot ROM installs bus-fault-ignore trap handler (same `_trap_bus_ignore` used for RAM probing)
7. Boot ROM reads `CFG_CLASS` at `0xFE00_0000` — if bus fault, no more devices → go to step 13
8. Boot ROM reads `CFG_SIZE`, `CFG_ID`, and `CFG_NAME0–3` to identify the device
9. Boot ROM assigns a base address (from available physical space), writes it to `CFG_BASE` at `0xFE00_001C`
10. Boot ROM toggles `CFG_EN`: clear then set (advances chain to next device)
11. Boot ROM prints device info to console
12. Go to step 7
13. Boot ROM disables config mode: `WRSYS SYSDEV_BUS, BUSCTL, 0` (clears CFG_EN)
14. Boot ROM restores normal bus fault handler
15. Boot ROM records the device table in the boot data structure (passed to stage 1 / kernel via R1)

```c
// C pseudocode for the autoconfig loop:
#define BUSCTL_RST    1
#define BUSCTL_CFG_EN 2

// Assert bus reset, delay, then enable config mode
penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_RST);
for (volatile int i = 0; i < 100; i++) {}  // delay for async bus
penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_CFG_EN);

TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;
int ndevs = 0;

for (;;) {
    uint32_t cls = bus_probe_read(0xFE000000);            // CFG_CLASS
    if (cls == 0xFFFFFFFF) break;                         // bus fault → no more devices
    uint32_t size = bus_probe_read(0xFE000004);           // CFG_SIZE
    uint32_t id   = bus_probe_read(0xFE000008);           // CFG_ID
    // ... read CFG_NAME0–3 ...

    uint32_t base = allocate_address(cls, size);
    *(volatile uint32_t *)0xFE00001C = base;              // CFG_BASE → device enables

    // Toggle CFG_EN to advance chain to next device
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, 0);       // clear CFG_EN
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_CFG_EN);  // set CFG_EN

    devtable[ndevs++] = (struct bootdev){ cls, id, base, size };
}

TRAP_VECTORS[TRAP_BUS_FAULT] = prev_handler;
penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, 0);           // disable config mode
```

### FPGA-Internal Autoconfig

FPGA-internal devices (soft peripherals, SDRAM controllers, etc.) participate in the same autoconfig protocol. The bus controller's `cfg` output feeds into the daisy chain, which is wired in RTL between module instances:

```systemverilog
// In machine_fpga.sv (or similar integration module):
// Bus controller (sysreg device 4) drives cfg and rst
assign bus_rst            = hw_rst | busctl_sw_rst;     // hardware OR software reset
assign fpga_spi_cfg_in   = busctl_cfg_out;              // first autoconfigured device
assign ext_bus_cfg        = fpga_spi_cfg_out;            // then to external bus connector
```

The config address range (`0xFE00_0000`) is decoded by the bus, and the `cfg` daisy chain determines which device responds. FPGA-internal and external discrete cards are indistinguishable from software's perspective — the same `LDW`/`STW` to config space discovers both.

### Expansion RAM

Expansion RAM cards are autoconfigured like any other device. They report `type=memory` and their installed size during config. The boot ROM assigns them contiguous addresses above system RAM (or in other available ranges). The OS kernel's memory map includes both system RAM and all expansion RAM regions.

This mirrors the Amiga model: Chip RAM (system RAM, hardwired at 0) + Zorro Fast RAM (expansion, autoconfigured).

## Arbitration

With two bus masters (CPU and DMA controller), the bus arbiter mediates access to the shared Penumbra Bus. The arbiter does not preempt while `req` is asserted, ensuring burst transfers complete atomically.

Arbitration policy TBD (round-robin or fixed-priority are the simplest starting points). The DMA controller should be able to burst transfer without per-word re-arbitration.

## FPGA and Discrete Implementation

The architecture maps to both implementation targets:

| Aspect | FPGA (ULX3S) | Discrete (74xx) |
|--------|-------------|-----------------|
| CPU clock | 25-50 MHz | 5-10 MHz (local crystal) |
| Internal bus | FPGA fabric wires (sync) | On-board traces (sync, local clock) |
| External bus | Penumbra Bus via GPIO pins | Penumbra Bus via backplane |
| System RAM | 32 MB SDRAM (onboard, hardwired at 0x0, size probed) | 1-2 MB SRAM (hardwired at 0x0, size probed) |
| I/O peripherals | Penumbra Bus via GPIO | Penumbra Bus via backplane |
| Async bridge | SystemVerilog module at GPIO boundary | Implicit — CPU board drives backplane directly |

### FPGA External Memory Validation

The FPGA build can optionally connect to **external SRAM via GPIO** to validate the Penumbra Bus timing and test memory board designs before the discrete build. Since the ULX3S has limited GPIO (~28 pin pairs), a **multiplexed address/data adapter** is used on this link only:

| Signal | Pins | Description |
|--------|------|-------------|
| `AD[31:0]` | 32 | Multiplexed address/data |
| `ALE` | 1 | Address latch enable — pulses to capture address |
| `we` | 1 | Write enable |
| `byte_en[3:0]` | 4 | Byte lane enables |
| `req` | 1 | Request |
| `ack` | 1 | Acknowledge |
| `bus_error` | 1 | Bus error |
| `data_dir` | 1 | Data bus direction |
| **Total** | **42** | Fits ULX3S GPIO comfortably |

The external memory board has a single 74x573 address latch that captures the address on `ALE`, then the same pins carry data for the transfer. This multiplexing exists **only** on the FPGA-to-external cable — it is not part of the Penumbra Bus standard.

On the FPGA, the onboard SDRAM and external SRAM are both bus devices with their own address comparators — the SDRAM claims its range on the internal sync bus, while the external SRAM responds on the Penumbra Bus via GPIO. Both can be active simultaneously for testing and comparison.

## Physical Memory Map

The physical address space uses a fixed layout decoded from the top address bits. RAM occupies the low addresses, I/O and boot ROM occupy the top.

### Top-Level Regions

```
0x0000_0000 ┌─────────────────────┐
            │ System RAM          │  Cached, hardwired at base 0
            │ FPGA: up to 32 MB  │  Size probed at boot (bus fault)
            │ Discrete: 1-2 MB   │
            ├─────────────────────┤  ← probed boundary
            │ Expansion RAM       │  Autoconfigured, assigned by boot ROM
            │ (optional)          │
            ├─────────────────────┤
            │ (unmapped)          │  Bus fault if accessed
0xFDFF_FFFF └─────────────────────┘
0xFE00_0000 ┌─────────────────────┐
            │ Config Space (16 B) │  Autoconfig registers (only when CFG_EN set)
0xFE00_000F └─────────────────────┘
            │ (unmapped)          │
0xFEFF_FFFF └─────────────────────┘
0xFF00_0000 ┌─────────────────────┐
            │ I/O Region (16 MB)  │  Always uncached (C=0)
            │ Hardwired: UART     │
            │ Autoconfigured: rest│
0xFFFE_FFFF └─────────────────────┘
0xFFFF_0000 ┌─────────────────────┐
            │ Boot ROM (64 KB)    │  Always uncached (C=0), hardwired
0xFFFF_FFFF └─────────────────────┘
```

- **Reset vector:** `0xFFFF_0000` (base of boot ROM). CPU starts here with MMU in flat mode (M=0). This is a hardwired PC reset value, not part of the vector table.
- **Exception vector table (MIPS/68k-style):** 16 words at physical `0x0000_0000` in RAM. Each entry contains a 32-bit handler address (not an instruction). The CPU reads the handler address from the vector table with MMU bypass, then jumps to that address. Software writes handler addresses at boot time via `STW`.
- **Unmapped regions:** Accessing unmapped addresses produces a bus fault (bus timeout, no device responds).
- **System RAM sizing:** System RAM is hardwired at address 0 and claims only its actual installed size. The boot ROM probes upward until bus fault to determine the boundary. Expansion RAM cards are autoconfigured and assigned addresses above system RAM (see [Device Discovery](#device-discovery-autoconfig)).

### I/O Peripheral Map

Within the 16 MB I/O region at `0xFF00_0000`:

| Base Address | Size | Peripheral | Hardwired/AC | Notes |
|-------------|------|------------|:---:|-------|
| `0xFF00_0000` | 4 KB | UART | HW | Serial console (NS16450-compatible) — **implemented in sim** |
| (assigned) | 4 KB | SPI controller | AC | SD card, flash |
| (assigned) | 4 KB | GPIO | AC | General-purpose I/O |
| (assigned) | 64 KB | Wiznet Ethernet | AC | Register + buffer window |
| `0xFF00_1000` - `0xFFFE_FFFF` | ~16 MB | (available) | — | Autoconfig assigns from this pool |

HW = hardwired (fixed address). AC = autoconfigured (address assigned at boot).

Only the console UART has a fixed I/O address. All other peripherals receive their addresses from the autoconfig boot sequence, which assigns them from the available I/O space.

Each simple peripheral gets a 4 KB page-aligned region. This is far more than needed (most use ~8 registers) but it means:
- Each peripheral occupies exactly one MMU page, so cacheability and permissions are per-device
- The autoconfig routine assigns page-aligned addresses naturally

### Address Decoding

Address decoding is **device-side**: each device on the bus contains its own address comparator and responds only to accesses within its claimed range. There is no central address decoder. This models how real shared buses work (ISA, VMEbus, S-100) — each card has its own address selection logic.

Each device uses the pattern `(addr & ~(size - 1)) == base` to check if an address falls within its window. Size must be a power of 2 and base must be naturally aligned. In the RTL simulation, this is implemented by the `bus_devsel` module (one instance per device, with elaboration-time assertions for power-of-2, alignment, and non-zero size).

**Hardwired devices** have fixed base addresses set at design time:

| Device | Base | Size | Notes |
|--------|------|------|-------|
| System RAM | `0x0000_0000` | Installed size | Probed at boot via bus fault |
| Console UART | `0xFF00_0000` | 4 KB | Must be available before autoconfig |
| Boot ROM | `0xFFFF_0000` | 64 KB | CPU starts here at reset |

**Autoconfigured devices** have their base addresses assigned at boot time by the autoconfig protocol (see [Device Discovery](#device-discovery-autoconfig)). They do not respond to normal bus cycles until configured.

Discrete decode cost per device: one 74x85 magnitude comparator on the upper address lines, with the number of compared bits determined by the device's address space size. Equivalent to DIP switches selecting the base address on an ISA card.

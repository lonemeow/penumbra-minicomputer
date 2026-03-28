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
│    ┌───────┴───────┐        │ Timer  │ │  DMA  │ │(future)│  │          │
│    │  MMU / TLB    │◄───────┤ dev 2  │ │ Ctrl  │ │ dev 4+)│  │          │
│    │  (dev 0)      │ sysreg └───┬────┘ │ dev 3 │ └────────┘  │          │
│    └───────┬───────┘            │      └───┬───┘             │          │
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
      ┌──────┴──────┐           │                                         │
      │   Address   │           │                                         │
      │   Decoder   │           │                                         │
      └──┬─────┬──┬─┘           │                                         │
         │     │  │              │                                         │
         v     v  v              │                                         │
┌──────┐┌────┐┌──────────────────┼────────────────────┐                    │
│Memory││Boot││   I/O Peripherals│                    │                    │
│SDRAM ││ROM ││                  │                    │                    │
│or    ││    ││┌──────┐┌─────┐┌──┴──┐┌───────┐┌─────┐│                    │
│SRAM  ││    │││ UART ││ SPI ││GPIO ││Wiznet ││(fut)││                    │
└──────┘└────┘│└──┬───┘└──┬──┘└──┬──┘└──┬────┘└─────┘│                    │
              └───┼───────┼──────┼──────┼─────────────┘                    │
                  └───┬───┴──┬───┴──────┘                                  │
                      v      v                                              │
            ┌──────────────────────┐     NMI ──────────────────────────────┘
            │  Priority Encoder    │──── IRQ + vector[2:0] ──> CPU
            │  (74x148 in discrete)│
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

**Total: 72 signals** (32 addr + 32 data + 8 control).

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

When an access hits unmapped address space, a **default slave** (part of the address decoder) asserts `bus_error` instead of `ack`. The CPU treats this as an exception.

In discrete, a simple timeout counter provides the same function: if `req` is asserted for N cycles of a local reference oscillator without `ack`, the counter asserts `bus_error`. This catches both unmapped addresses and hung peripherals.

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

| Signal | Width | Direction | Description |
|--------|-------|-----------|-------------|
| `irq` | 1 | Priority Encoder → CPU | At least one unmasked interrupt pending |
| `irq_vector[2:0]` | 3 | Priority Encoder → CPU | Highest-priority pending device number |
| `nmi` | 1 | Direct → CPU | Non-maskable interrupt (debug, critical fault) |
| `irq_lines[7:0]` | 8 | Peripherals → Priority Encoder | Individual device interrupt requests |

Priority is fixed by wiring order to the encoder. Suggested priority (highest first):
1. Timer (drives Minix 2 scheduler)
2. UART
3. Wiznet Ethernet
4. DMA complete
5. SPI/SD
6. (reserved)
7. (reserved)
8. (reserved)

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

Shared-bus connecting via the Penumbra Bus:
- CPU complex (bus master, via async bridge)
- DMA controller (bus master)
- Memory (SDRAM on FPGA, SRAM in discrete)
- Boot ROM
- UART (serial console)
- GPIO
- SPI (SD card, other peripherals)
- Wiznet Ethernet adapter (MMIO + DMA)

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
| Main memory | 32 MB SDRAM (onboard, via internal sync bus) | 1-2 MB SRAM (via Penumbra Bus) |
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

The address decoder on the FPGA routes physical addresses to either the onboard SDRAM (internal sync bus) or the external SRAM (via async bridge + GPIO), allowing both memory paths to be active simultaneously for testing and comparison.

## Physical Memory Map

The physical address space uses a fixed layout decoded from the top address bits. RAM occupies the low addresses, I/O and boot ROM occupy the top.

### Top-Level Regions

```
0x0000_0000 ┌─────────────────────┐
            │ RAM                 │  Cached (normal operation)
            │ FPGA: 32 MB SDRAM  │
            │ Discrete: 1-2 MB   │
0x01FF_FFFF │   SRAM             │
            └─────────────────────┘
0x0200_0000 ┌─────────────────────┐
            │ (unmapped)          │  Bus fault if accessed
0xFEFF_FFFF └─────────────────────┘
0xFF00_0000 ┌─────────────────────┐
            │ I/O Region (16 MB)  │  Always uncached (C=0)
0xFFFF_DFFF └─────────────────────┘
0xFFFF_E000 ┌─────────────────────┐
            │ Boot ROM (8 KB)     │  Always uncached (C=0)
0xFFFF_FFFF └─────────────────────┘
```

- **Reset vector:** `0xFFFF_E000` (base of boot ROM). CPU starts here with MMU in flat mode (M=0).
- **Exception vector table:** Fixed physical addresses at `0x0000_0000` in RAM. Vector fetches bypass the MMU.
- **Unmapped regions:** Accessing unmapped addresses produces a bus fault exception.

### I/O Peripheral Map

Within the 16 MB I/O region at `0xFF00_0000`:

| Base Address | Size | Peripheral | Notes |
|-------------|------|------------|-------|
| `0xFF00_0000` | 4 KB | UART | Serial console |
| `0xFF00_1000` | 4 KB | SPI controller | SD card, flash |
| `0xFF00_2000` | 4 KB | GPIO | General-purpose I/O |
| `0xFF00_3000` - `0xFF00_FFFF` | 52 KB | (reserved) | Future simple peripherals |
| `0xFF01_0000` | 64 KB | Wiznet Ethernet | Register + buffer window |
| `0xFF02_0000` - `0xFFFF_DFFF` | ~15.8 MB | (reserved) | Future expansion |

Each simple peripheral gets a 4 KB page-aligned region. This is far more than needed (most use ~8 registers) but it means:
- Each peripheral occupies exactly one MMU page, so cacheability and permissions are per-device
- Address decoding within the I/O block uses `addr[19:12]` to select the device — one 74x138 decoder in discrete

### Address Decoding

Top-level decode checks the top 8 address bits:

| `addr[31:24]` | Region |
|---------------|--------|
| `0x00` - `0x01` | RAM (32 MB max) |
| `0x02` - `0xFE` | Unmapped (bus fault) |
| `0xFF` | I/O + Boot ROM |

Within the `0xFF` region, `addr[23:17]` distinguishes I/O (lower) from Boot ROM (upper, `0xFFFF_E000`+).

Discrete decode cost: ~3-4 chips (one 74x85 comparator for RAM range, one for the 0xFF block, one 74x138 decoder for peripheral select).

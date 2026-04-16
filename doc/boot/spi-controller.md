# SPI Controller Design

## Overview

A SPI master peripheral for the Penumbra bus. The primary use case is SD card access (SPI mode) for the boot chain and NetBSD root filesystem, but the controller is generic enough for other SPI devices (ESP32 WiFi module, etc.).

The hardware provides a shift register with configurable clock speed, chip-select control, and a 16550-style optional hardware FIFO with a transfer engine for autonomous burst transfers. All SD protocol logic (commands, response parsing, block framing) lives in firmware/driver software.

### Design History

The original v1 controller was byte-at-a-time polled with 4 registers (DATA, STATUS, CONTROL, CLKDIV). The v2 redesign replaces the register layout entirely (no backward-compatible legacy registers) with a unified interface that supports both polled single-byte mode and FIFO-driven burst mode via a 16550-style FIFO enable bit. All software consumers (boot ROM `spi.h`, NetBSD `psd` driver) are updated to the new layout.

## NetBSD Driver Strategy

NetBSD has no in-tree SPI-mode SD driver. The SD/MMC stack is:

```
 ld(4) block device
   │
 sdmmc(4) protocol layer  ← card discovery, CSD/CID, block I/O
   │
 host controller driver    ← sends/receives bytes over hardware
   │
 hardware
```

We will write a custom host controller driver (`penspi(4)`) that implements `sdmmc_chip_functions` by talking to the SPI master registers. The driver is small (~500 lines) because `sdmmc(4)` handles all the SD protocol complexity — the host driver just sends and receives bytes.

The standard SDHCI (SD Host Controller Interface) register set has ~30 registers and requires DMA, power sequencing, and clock management. It is far too complex for our purposes and for a future 74xx build.

## Bus Integration

The SPI controller is an **autoconfigured** device — it does not have a hardwired address. At boot, the ROM's autoconfig routine discovers it via the daisy-chained `cfg` protocol (see `doc/bus/bus-overview.md`) and assigns it a base address. The driver discovers the assigned base address through autoconfig, not a compile-time constant.

This means a board without an SD card (or with a different storage device) simply doesn't have this device on the bus — the autoconfig enumeration skips it, and the boot ROM falls back to serial upload or another boot method.

The simulation (`machine_sim.sv`) implements the same autoconfig protocol as real hardware — the SPI controller participates in the `cfg` daisy chain and receives its address from the boot ROM's autoconfig code.

**Autoconfig identity:**
- Class: `CLASS_SD` (4) — SD/MMC card slot (SPI register interface, CS0 = card). A generic SPI controller without an attached SD card would use `CLASS_SPI` (3) instead; the distinction allows the ROM to know which devices can be used for booting without blind probing.
- Required size: 4 KB (one page)
- Name: `"SD"` (16 bytes, null-padded)
- ID: 0 (generic — any SPI master with this register layout is compatible)

## Register Interface

Seven word-strided registers. Same bus protocol as `sim_uart`: 1-cycle read latency, 0-cycle write. Base address assigned by autoconfig.

Registers are ordered identification-first: CAP and STATUS (read-only, safe to probe) come before CONTROL and DATA (side-effects on write). This makes raw-address debugging straightforward — examining registers sequentially from the base address goes from "what is this device" to "what's its state" to "how to configure it" to "data transfer."

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| `0x00` | CAP | R | Capability/identification: version, FIFO depth |
| `0x04` | STATUS | R | Transfer state, FIFO levels and flags |
| `0x08` | CONTROL | R/W | CS pins, SPI mode, clock speed, FIFO enable, flush |
| `0x0C` | DATA | R/W | TX/RX data byte (single-byte or FIFO, per FIFO_EN) |
| `0x10` | XFER_COUNT | R/W | Autonomous transfer: byte count + START |
| `0x14` | IRQ_STATUS | R/W | Interrupt status (mixed latched + live) |
| `0x18` | IRQ_ENABLE | R/W | Per-source interrupt mask |

Address decode uses `addr[4:2]` (3 bits, 7 of 8 slots used).

### CAP (0x00) — Capability Register (Read-Only)

```
Bits [7:0]   — Version: 1 = SPI v2 with FIFO support
Bits [23:8]  — FIFO depth (entry count, same for TX and RX)
Bits [31:24] — Reserved (0)
```

Software reads CAP to detect hardware capabilities. Version 0 (or read-as-zero for unmapped registers on older hardware) indicates a v1 controller without FIFO support. The FIFO depth field reports the actual hardware depth, parameterized at instantiation — software adapts its transfer strategy accordingly.

### STATUS (0x04) — Status Register (Read-Only)

```
Bit  [0]     — SPI_BUSY: shift register transfer in progress
Bit  [1]     — SPI_DONE: last single-byte transfer complete (RX valid)
Bits [3:2]   — Reserved (0)
Bits [15:4]  — TX_LEVEL: number of bytes in TX FIFO (12 bits, 0–4095)
Bits [27:16] — RX_LEVEL: number of bytes in RX FIFO (12 bits, 0–4095)
Bit  [28]    — TX_EMPTY: TX FIFO is empty
Bit  [29]    — TX_FULL: TX FIFO is full
Bit  [30]    — RX_EMPTY: RX FIFO is empty
Bit  [31]    — RX_FULL: RX FIFO is full
```

When FIFO_EN=0, the FIFO level and flag fields read as zero (FIFOs inactive). SPI_BUSY and SPI_DONE reflect single-byte transfers exactly as in v1.

When FIFO_EN=1, SPI_BUSY reflects the transfer engine state (busy while XFER_COUNT > 0 and bytes are being clocked). SPI_DONE is not meaningful in FIFO mode — use XFER_DONE in IRQ_STATUS instead.

### CONTROL (0x08) — Control Register (Read/Write)

```
Bit  [0]     — CS0: chip select 0 (directly driven, active-low on pin)
Bit  [1]     — CS1: chip select 1
Bits [3:2]   — Reserved (0)
Bit  [4]     — CPOL: clock polarity (0=idle low, 1=idle high)
Bit  [5]     — CPHA: clock phase (0=sample leading edge, 1=trailing)
Bit  [6]     — FAST: clock speed select (0=slow ≤400kHz, 1=fast)
Bit  [7]     — FIFO_EN: enable FIFO mode (16550-style)
Bits [13:8]  — Reserved (0)
Bit  [14]    — FLUSH_TX: write 1 to clear TX FIFO (self-clearing)
Bit  [15]    — FLUSH_RX: write 1 to clear RX FIFO (self-clearing)
Bits [31:16] — Reserved (0)
```

**Reset default:** `0x03` (both CS deasserted, mode 0, slow clock, FIFO disabled).

**CS pins:** Software asserts/deasserts CS explicitly (not automatic per-transfer). This is necessary for SD-SPI where CS must stay asserted across multi-byte command/response sequences. Write 0 to assert, 1 to deassert.

**FAST/SLOW clock:** Two hardware-parameterized clock speeds. SLOW (default) provides ≤400 kHz for SD card initialization. FAST provides the maximum operational speed (system-clock dependent — typically 6.25 MHz at 12.5 MHz system clock). Hardware parameters `SLOW_DIV` and `FAST_DIV` set the actual divider values at instantiation; software never computes clock dividers. This avoids coupling software to knowledge of the SPI controller's input clock frequency, which may differ from the CPU clock if the SPI controller is on a separate clock domain (e.g., on the async external bus in a future discrete build).

**FIFO_EN:** When clear (reset default), DATA works in single-byte polled mode — identical to the original v1 controller behavior. When set, DATA pushes to/pops from hardware FIFOs, and the transfer engine (XFER_COUNT) is available for autonomous burst transfers. The boot ROM never sets FIFO_EN, so it works without modification.

**FLUSH_TX / FLUSH_RX:** Write 1 to reset the corresponding FIFO to empty. These bits are self-clearing — they always read as 0. Only effective when FIFO_EN=1.

### DATA (0x0C) — Data Register (Read/Write)

**When FIFO_EN=0 (single-byte mode):**
- Write: starts an 8-bit SPI transfer. The written byte is shifted out on MOSI (MSB first) while simultaneously shifting in from MISO. When complete (8 clock cycles), SPI_BUSY clears, SPI_DONE sets, and the received byte is available by reading DATA.
- Read: returns the last received byte.
- Writing while SPI_BUSY is undefined.

**When FIFO_EN=1 (FIFO mode):**
- Write: pushes a byte into the TX FIFO. Does not start a transfer — use XFER_COUNT to start the engine.
- Read: pops a byte from the RX FIFO. Returns the oldest received byte.
- Writing to a full TX FIFO or reading from an empty RX FIFO is undefined — software must check STATUS first.

In both modes, data occupies bits [7:0]; bits [31:8] read as zero.

### XFER_COUNT (0x10) — Transfer Count Register (Read/Write)

```
Bits [15:0]  — COUNT: number of bytes to transfer (0–65535)
Bit  [16]    — START: write 1 to begin autonomous transfer
Bits [31:17] — Reserved (0)
```

Only effective when FIFO_EN=1. Writing with START=1 initiates an autonomous burst transfer:

1. The transfer engine waits for the TX FIFO to have data (stalls if empty).
2. Pops a byte from the TX FIFO and loads the shift register.
3. Shifts 8 bits through the SPI shift register (8 clock cycles at the currently selected speed).
4. Waits for the RX FIFO to have space (stalls if full).
5. Pushes the received byte into the RX FIFO. COUNT decrements.
6. If COUNT > 0, go to step 1.
7. If COUNT = 0, the engine stops and asserts XFER_DONE in IRQ_STATUS.

Reading XFER_COUNT returns the remaining byte count (useful for diagnostics).

**Interlock with single-byte mode:** If a legacy single-byte transfer is in progress (SPI_BUSY from a DATA write before FIFO_EN was set), the engine waits for it to complete before starting. This prevents bus contention on the shift register.

**TX empty stall:** If the TX FIFO is empty when the engine needs the next byte, the SPI clock pauses and the engine waits. The TX_THRESH watermark IRQ fires (tx_level <= half), the handler refills the TX FIFO, and the engine resumes automatically. This prevents data corruption — no 0xFF padding bytes are ever substituted. Software must ensure the TX FIFO is pre-filled or watermark-driven for the full transfer count.

**RX full stall:** If the RX FIFO is full when the engine needs to store a received byte, the SPI clock pauses and the engine waits. The RX_THRESH watermark IRQ fires (rx_level >= half), the handler drains the RX FIFO, and the engine resumes automatically. This prevents data loss — no received bytes are ever dropped.

**Why stalling is safe:** SPI is a master-clocked protocol — the master controls SCK and can pause it indefinitely between bytes. SD cards in SPI mode have no timeout on clock gaps between bytes (the spec only requires minimum clock frequency during initialization). Stalling adds latency but never corrupts data, which is the correct tradeoff for a block device backing a filesystem.

### IRQ_STATUS (0x14) — Interrupt Status Register (Read / Partial W1C)

```
Bit [0] — XFER_DONE:  transfer engine completed (latched, write-1-to-clear)
Bit [1] — RX_THRESH:  RX FIFO level >= half depth (live, read-only)
Bit [2] — TX_THRESH:  TX FIFO level <= half depth (live, read-only)
```

**Mixed latched and live semantics** — this is critical for correct interrupt handling:

- **XFER_DONE (bit 0):** Latched on the rising edge (engine count reaches zero). Cleared by writing 1 to bit 0. This is a one-shot event per transfer — once cleared, it cannot re-trigger until a new transfer starts via XFER_COUNT. No race condition is possible.

- **RX_THRESH (bit 1):** Live level-sensitive signal. Asserted whenever the RX FIFO level is at or above half the FIFO depth. Not clearable by software — de-asserts automatically when the handler drains the RX FIFO below the threshold. If the handler does not drain enough, the IRQ stays asserted and the handler re-enters. **No lost interrupts possible.**

- **TX_THRESH (bit 2):** Live level-sensitive signal. Asserted whenever the TX FIFO level is at or below half the FIFO depth. De-asserts automatically when software fills the TX FIFO above the threshold.

Writing to this register only affects bit 0 (XFER_DONE). Bits 1–2 are read-only.

### IRQ_ENABLE (0x18) — Interrupt Enable Register (Read/Write)

```
Bit [0] — XFER_DONE enable
Bit [1] — RX_THRESH enable
Bit [2] — TX_THRESH enable
```

**IRQ output:**

```
o_irq = (IRQ_STATUS[0] & IRQ_ENABLE[0])    // XFER_DONE (latched)
      | (RX_THRESH     & IRQ_ENABLE[1])     // RX level (live)
      | (TX_THRESH     & IRQ_ENABLE[2])     // TX level (live)
```

The `o_irq` output feeds into the shared wired-OR `/IRQ` line (see `doc/bus/bus-overview.md`). There is no interrupt controller — the CPU's IRQ handler polls each device's IRQ_STATUS to identify the source.

**Reset default:** `0x00` (all interrupts disabled).

## Interrupt Handling — Correct Usage

### Polling mode (boot ROM)

The boot ROM never enables interrupts. It polls STATUS for SPI_BUSY/SPI_DONE in single-byte mode:

```
CONTROL = 0x02;        // assert CS0, slow clock, no FIFO
DATA = cmd_byte;       // start transfer
while (STATUS & 1) {}  // poll SPI_BUSY
rx = DATA;             // read received byte
```

### FIFO burst mode — large FIFO (kernel, FIFO >= sector size)

When the FIFO is large enough to hold a full transfer (e.g., 512 bytes on FPGA):

```
CONTROL |= FIFO_EN | FAST;       // enable FIFO, fast clock
// Fill TX FIFO with all bytes for the transfer
for (i = 0; i < 6; i++) DATA = cmd[i];   // 6-byte SD command
for (i = 0; i < 514; i++) DATA = 0xFF;   // padding (read) or data+CRC (write)
XFER_COUNT = 520 | START;                 // total byte count
IRQ_ENABLE = XFER_DONE_EN;               // enable completion interrupt

// ... CPU does other work ...

// IRQ handler:
status = IRQ_STATUS;
if (status & XFER_DONE) {
    IRQ_STATUS = XFER_DONE;              // W1C: clear the flag
    // Skip command echo bytes, read sector data
    for (i = 0; i < 6; i++) DATA;       // discard command echo
    // ... find data token, read 512 bytes ...
}
```

Note: the TX FIFO must be pre-filled with the full transfer count. The engine stalls if the TX FIFO empties — it will not substitute padding bytes.

### FIFO burst mode — small FIFO (kernel, FIFO < sector size)

When the FIFO is smaller than the transfer (e.g., 32 bytes on discrete 74xx), the kernel uses watermark interrupts to keep the FIFOs fed and drained. The engine stalls safely if either FIFO hits its limit, then resumes when the handler services it:

```
CONTROL |= FIFO_EN | FAST;
// Pre-fill TX FIFO to capacity
while (!(STATUS & TX_FULL)) DATA = next_tx_byte();
XFER_COUNT = 520 | START;
IRQ_ENABLE = XFER_DONE_EN | RX_THRESH_EN | TX_THRESH_EN;

// IRQ handler:
status = IRQ_STATUS;
// Drain RX if above threshold — engine may be stalled waiting for space
if (status & RX_THRESH) {
    while (!(STATUS & RX_EMPTY)) buf[rx_idx++] = DATA;
}
// Refill TX if below threshold — engine may be stalled waiting for data
if (status & TX_THRESH) {
    while (!(STATUS & TX_FULL) && tx_remaining > 0) {
        DATA = next_tx_byte();
        tx_remaining--;
    }
}
// Transfer complete (latched — must W1C)
if (status & XFER_DONE) {
    IRQ_STATUS = XFER_DONE;
    // Drain any remaining RX bytes
    while (!(STATUS & RX_EMPTY)) buf[rx_idx++] = DATA;
    signal_completion();
}
```

**Why this is correct by construction:**

- **No data loss:** The engine stalls when the RX FIFO is full — received bytes are never dropped. The RX_THRESH IRQ fires, the handler drains, and the engine resumes.
- **No data corruption:** The engine stalls when the TX FIFO is empty — no padding bytes are ever sent. The TX_THRESH IRQ fires, the handler refills, and the engine resumes. This eliminates the write-corruption-on-power-loss window that would exist if the engine substituted 0xFF on underrun.
- **No lost interrupts:** The watermark conditions (RX_THRESH, TX_THRESH) are live level signals, not latched. They cannot be "lost" — if the condition is still true after servicing, the IRQ stays asserted and the handler re-enters. XFER_DONE is a one-shot latch that cannot re-trigger until a new transfer starts.

## Hardware Implementation

### Block Diagram

```
                  Bus Interface
                 ┌──────────────────────────────┐
  mem_addr ─────►│ Register decode (addr[4:2])   │
  mem_wdata ────►│  CAP / STATUS / CONTROL /     │
  mem_we ───────►│  DATA / XFER_COUNT /          │
  mem_re ───────►│  IRQ_STATUS / IRQ_ENABLE      │
  mem_rdata ◄───│                                │
  mem_busy ◄────│                                │
                 └─────┬──────────┬──────────────┘
                       │          │
              ┌────────▼──┐  ┌───▼──────────┐
              │  TX FIFO  │  │   RX FIFO    │
              │ (param    │  │  (param      │
              │  depth)   │  │   depth)     │
              └────┬──────┘  └───▲──────────┘
                   │             │
              ┌────▼─────────────┴──────────┐
              │     Transfer Engine          │
              │  ┌────────────────────────┐  │
              │  │  SPI Shift Register    │  │
              │  │  8-bit, MSB first      │  │
              │  └──────────┬─────────────┘  │
              │  FAST/SLOW clock divider     │
              │  Byte counter (XFER_COUNT)   │
              │  CPOL/CPHA logic             │
              └────────────┬────────────────┘
                           │
               ┌───────────┼───────────┐
               │           │           │
            o_sclk     o_mosi      i_miso
               │
            o_cs0, o_cs1

                     ┌──────────────┐
  o_irq ◄──────────│  IRQ Logic   │
                     │  XFER_DONE   │◄── engine complete (latched, W1C)
                     │  RX_THRESH   │◄── rx_level >= depth/2 (live)
                     │  TX_THRESH   │◄── tx_level <= depth/2 (live)
                     │  & IRQ_ENABLE│
                     └──────────────┘
```

### Transfer Engine State Machine

The engine has four states. It stalls in LOAD_BYTE if the TX FIFO is empty, and in STORE_BYTE if the RX FIFO is full — the SPI clock pauses during stalls.

```
           ┌─────────┐
    ───────► S_IDLE  │ XFER_COUNT write with START=1
           │         ├──────────────────────┐
           └────▲────┘                      ▼
                │                    ┌──────────────┐
                │                    │ S_LOAD_BYTE  │ Pop TX FIFO
                │                    │ (stall if    │ Load shift register
                │                    │  TX empty)   │
                │                    └──────┬───────┘
                │                           ▼
                │                    ┌──────────────┐
                │                    │ S_SHIFTING   │ Clock 8 bits
                │                    └──────┬───────┘
                │                           ▼
                │                    ┌──────────────┐
                │  count=0           │ S_STORE_BYTE │ Push RX byte
                ├────────────────────│ (stall if    │ Decrement count
                │                    │  RX full)    │
                │                    └──────┬───────┘
                │                           │ count>0
        set XFER_DONE              (back to S_LOAD_BYTE)
```

In S_IDLE, single-byte DATA writes trigger the shift register directly (bypassing the engine) when FIFO_EN=0. When FIFO_EN=1, DATA writes push to the TX FIFO and the engine must be started via XFER_COUNT.

### 74xx Feasibility

The SPI core (shift register + clock divider + control) is ~4 chips:
- 74HC595 or 74HC165: 8-bit shift register (TX or combined TX/RX)
- 74HC4040: clock divider counter (FAST/SLOW selects tap)
- 74HC74: control flip-flops (BUSY, CS, CPOL/CPHA, FIFO_EN)
- 74HC00/74HC32: gating logic (clock enable, CS mux)

The FIFO adds ~2 chips per direction for a small discrete FIFO (e.g., 74HC40105 16-deep FIFO × 2 for TX/RX, or a small SRAM + counter pair). The transfer engine byte counter is one more counter chip. The live watermark comparators for IRQ are magnitude comparators (74HC85) — one per FIFO, comparing the level against the hardwired half-depth threshold.

Total for a 16-byte FIFO build: ~12 chips. The register interface and bus decode add a few more.

## Simulation Model

### RTL: `sim_spi.sv`

The simulation SPI controller implements the same register interface as the real hardware. Transfers complete after `SPI_BUSY_CYCLES` (default 2) instead of real shift-register clocking. In FIFO mode, the transfer engine processes one byte every `SPI_BUSY_CYCLES` cycles.

On the external side, it exposes signal-level ports to the testbench (same pattern as `sim_uart.sv`):

```systemverilog
module sim_spi (
    // Bus interface
    input  logic        i_clk, i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we, i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // SPI signals exposed to testbench
    output logic        o_cmd_valid,   // Pulses when a byte is shifted
    output logic [7:0]  o_cmd_data,    // Byte being sent (MOSI)
    input  logic        i_resp_valid,  // Testbench presents response byte
    input  logic [7:0]  i_resp_data,   // Byte from testbench (MISO)
    output logic        o_cs0,
    output logic        o_cs1,

    // Interrupt output
    output logic        o_irq
);
```

### Testbench: SD card emulator

In `tb_interactive.cpp`, the testbench monitors the SPI signals and emulates an SD card backed by a disk image file:

```
+sdcard=disk.img     (Verilator plusarg for disk image path)
```

The testbench implements the SD-SPI state machine:
- Tracks CS0 assertion
- Recognizes CMD0, CMD8, CMD17 (read), CMD24 (write), CMD55+ACMD41
- On CMD17: `fseek(img, lba * 512)`, `fread()` 512 bytes, feeds back via `i_resp_data`
- Generates correct R1 responses, data tokens (0xFE), and dummy CRC

## SD-SPI Software Flow

### Initialization (boot ROM, polled, no FIFO)

```
1. CONTROL = 0x03 (CS deasserted, slow clock, no FIFO, mode 0)
2. Send 80+ clock cycles with CS deasserted (shift 0xFF × 10 via DATA)
3. Assert CS0: CONTROL = 0x02
4. Send CMD0 (GO_IDLE_STATE): 6 bytes via DATA, poll STATUS each byte
5. Poll for R1 response (shift 0xFF via DATA, check for non-0xFF)
6. Send CMD8, ACMD41 initialization sequence
7. Switch to fast clock: CONTROL = 0x42 (CS0 asserted, FAST=1)
8. CMD17 (READ_SINGLE_BLOCK) for sector reads:
   a. Send command (6 bytes)
   b. Poll for data token (0xFE)
   c. Read 512 bytes + 2 CRC bytes
9. Deassert CS0 when done: CONTROL = 0x43
```

### Kernel bulk I/O (FIFO mode)

```
1. Read CAP to discover FIFO depth
2. CONTROL |= FIFO_EN | FAST
3. For each sector read:
   a. Fill TX FIFO with CMD17 bytes
   b. XFER_COUNT = transfer_size | START
   c. If FIFO >= sector: wait for XFER_DONE, drain RX FIFO
   d. If FIFO < sector: use watermark IRQs for streaming
4. Deassert CS0 between commands as needed
```

## Address Map

The SPI controller's base address is assigned at boot by autoconfig. There is no fixed address — the ROM's autoconfig routine chooses where to place it based on device type and available address space.

## Design Decisions

1. **16550-style FIFO enable:** A single FIFO_EN bit in CONTROL switches DATA between single-byte and FIFO mode. The ROM never sets this bit, so it works with the default polled path unchanged. The kernel enables it after detecting FIFO support via CAP. This is the same pattern as the 16550 UART's FCR bit 0.

2. **FAST/SLOW instead of CLKDIV:** SD-SPI has exactly two speed phases: ≤400 kHz init and maximum operational speed. A raw clock divider register would require software to know the SPI controller's input clock frequency, which creates an undesirable coupling — especially if the SPI controller moves to a separate clock domain on the async external bus. Two hardware-parameterized speeds (`SLOW_DIV`, `FAST_DIV`) eliminate this entirely.

3. **Mixed IRQ semantics:** XFER_DONE is latched (W1C) because it's a one-shot event. Watermark interrupts (RX_THRESH, TX_THRESH) are live level signals because they represent ongoing conditions — latching them creates race windows where interrupts can be lost. This split follows the PL022 / Designware SSI pattern.

4. **Stall-on-empty/full, never substitute or drop:** The engine pauses the SPI clock when the TX FIFO empties or the RX FIFO fills. SPI is master-clocked, so pausing SCK between bytes is always safe — SD cards have no inter-byte timeout in SPI mode. This eliminates silent data corruption (TX underrun sending 0xFF during a write) and silent data loss (RX overrun dropping bytes during a read). The watermark IRQs keep the FIFOs fed/drained so stalls are brief. For SD reads, software must fill the TX FIFO with 0xFF padding explicitly — the engine does not auto-generate it.

5. **Autoconfig, not hardwired:** The SPI controller participates in the bus autoconfig protocol. Boards without SD cards simply omit the device.

6. **Same register layout for real and simulation:** `sim_spi.sv` and `spi.sv` implement the identical 7-register interface. The simulation model skips the shift register and clock divider — transfers complete in configurable cycles.

## Module Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `FIFO_DEPTH` | 512 | TX and RX FIFO depth (must be power of 2). FPGA: 512+. Discrete 74xx: 16–32. |
| `SLOW_DIV` | 63 | Clock divider for SLOW mode. SPI_CLK = CLK / (2 × (div + 1)). |
| `FAST_DIV` | 0 | Clock divider for FAST mode. 0 = CLK/2 (maximum speed). |

## Open Questions

1. **DMA:** Eventually we want DMA for bulk SD transfers. The FIFO-based engine reduces urgency — a 512-byte FIFO handles a full sector without CPU interaction. DMA deferred until the DMA controller design.

2. **CS pin count:** Two CS pins (SD + one expansion) should be enough. More can be added via a separate GPIO or decode chip.

3. **Watermark configurability:** Currently the watermark threshold is hardwired at half the FIFO depth. A configurable threshold register could be added later if needed (e.g., for very asymmetric TX/RX patterns), but half-depth is the standard default and sufficient for SD card I/O.

# SPI Controller Design

## Overview

A minimal SPI master peripheral for the Penumbra bus. The primary use case is SD card access (SPI mode) for the boot chain and NetBSD root filesystem, but the controller is generic enough for other SPI devices (ESP32 WiFi module, etc.).

The hardware is intentionally minimal — a shift register with clock divider and chip-select control. All protocol logic (SD commands, response parsing, block transfers) lives in firmware/driver software.

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

The SPI controller is an **autoconfigured** device — it does not have a hardwired address. At boot, the ROM's autoconfig routine discovers it via the daisy-chained `cfg` protocol (see `doc/bus/bus-overview.md`) and assigns it a base address. The ROM chooses the address based on the device's reported type and size — I/O peripherals typically land in the high I/O region, but this is a software policy decision, not a hardware constraint. The driver discovers the assigned base address through autoconfig, not a compile-time constant.

This means a board without an SD card (or with a different storage device) simply doesn't have this device on the bus — the autoconfig enumeration skips it, and the boot ROM falls back to serial upload or another boot method.

The simulation (`machine_sim.sv`) implements the same autoconfig protocol as real hardware — the SPI controller participates in the `cfg` daisy chain and receives its address from the boot ROM's autoconfig code. This ensures the full discovery path is tested in simulation.

**Autoconfig identity:**
- Class: `CLASS_SERIAL` (1) — byte-oriented I/O controller
- Required size: 4 KB (one page)
- Name: `"SPI"` (16 bytes, null-padded)
- ID: 0 (generic — any SPI master with this register layout is compatible)

## Register Interface

Word-strided, data in bits [7:0] or [31:0] as noted. Same bus protocol as `sim_uart`: 1-cycle read latency, 0-cycle write. Base address assigned by autoconfig.

| Offset | Name | R/W | Width | Description |
|--------|------|-----|-------|-------------|
| `0x00` | DATA | R/W | 8 | TX/RX data. Write starts a transfer (shifts out written byte while shifting in from MISO). Read returns last received byte. |
| `0x04` | STATUS | R | 8 | Bit 0: BUSY (transfer in progress). Bit 1: DONE (transfer complete, RX data valid). |
| `0x08` | CONTROL | R/W | 8 | Bit 0: CS0 (directly drives chip select 0, active low). Bit 1: CS1. Bit 4: CPOL. Bit 5: CPHA. |
| `0x0C` | CLKDIV | R/W | 16 | Clock divider. SPI clock = system clock / (2 × (CLKDIV + 1)). Reset default: 0xFF (slow clock for SD card init). |

### Register Details

**DATA (0x00):** Writing a byte initiates an 8-bit SPI transfer. The written byte is shifted out on MOSI (MSB first) while simultaneously shifting in from MISO. When the transfer completes (8 clock cycles later), BUSY clears, DONE sets, and the received byte is available by reading DATA. Writing while BUSY is undefined (software must poll STATUS first).

**STATUS (0x04):**
- Bit 0 — BUSY: Set when a transfer is in progress. Cleared when complete.
- Bit 1 — DONE: Set when a transfer completes. Cleared when DATA is written (new transfer starts).

Polling loop: `while (STATUS & 1) {}; rx_byte = DATA;`

**CONTROL (0x08):** Direct chip-select control. Software asserts/deasserts CS explicitly (not automatic per-transfer). This is necessary for SD-SPI where CS must stay asserted across multi-byte command/response sequences.
- Bits [1:0] — CS pins (directly driven, directly active-low on the pin — write 0 to assert, 1 to deassert). CS0 = SD card. CS1 = reserved (ESP32 or expansion).
- Bit 4 — CPOL: Clock polarity (0 = idle low, 1 = idle high). SD cards use mode 0 (CPOL=0, CPHA=0).
- Bit 5 — CPHA: Clock phase (0 = sample on leading edge, 1 = trailing edge).

Reset default: `0x03` (both CS deasserted, mode 0).

**CLKDIV (0x0C):** SD cards require ≤400 kHz during initialization, then can be switched up to 25 MHz. At 25 MHz system clock: CLKDIV=31 gives ~400 kHz init clock, CLKDIV=0 gives 12.5 MHz operational clock.

### SD-SPI Software Flow (firmware/driver)

```
1. Set CLKDIV for ≤400 kHz
2. Send 80+ clock cycles with CS deasserted (shift 0xFF bytes × 10)
3. Assert CS0 (CONTROL bit 0 = 0)
4. Send CMD0 (GO_IDLE_STATE): 0x40, 0x00, 0x00, 0x00, 0x00, 0x95
5. Poll for R1 response (shift 0xFF, check for non-0xFF)
6. Send CMD8, ACMD41 initialization sequence
7. Set CLKDIV for operational speed
8. CMD17 (READ_SINGLE_BLOCK) for sector reads:
   a. Send command (6 bytes)
   b. Poll for data token (0xFE)
   c. Read 512 bytes + 2 CRC bytes
9. Deassert CS0 when done
```

All of this logic lives in the boot ROM (for stage 1 loading), the stage 1 bootloader (for FAT32 + stage 2 loading), and the NetBSD `penspi(4)` driver. The hardware just shifts bytes.

## Hardware Implementation

### Block Diagram

```
                  Bus Interface
                 ┌─────────────────────┐
  mem_addr ─────►│ Register decode     │
  mem_wdata ────►│  DATA / STATUS /    │
  mem_we ───────►│  CONTROL / CLKDIV   │◄──── RX shift register
  mem_re ───────►│                     ├────► TX shift register
  mem_rdata ◄───│                     │
  mem_busy ◄────│                     │
                 └────────┬────────────┘
                          │
                 ┌────────▼────────────┐
                 │ SPI Master Engine   │
                 │  8-bit shift reg    │
                 │  3-bit bit counter  │
                 │  Clock divider      │
                 │  CPOL/CPHA logic    │
                 └────────┬────────────┘
                          │
              ┌───────────┼───────────┐
              │           │           │
           o_spi_clk  o_spi_mosi  i_spi_miso
              │           │           │
           o_spi_cs0  o_spi_cs1
```

### 74xx Feasibility

The SPI master is ~4 chips:
- 74HC595 or 74HC165: 8-bit shift register (TX or combined TX/RX)
- 74HC4040: clock divider counter
- 74HC74: control flip-flops (BUSY, CS, CPOL/CPHA)
- 74HC00/74HC32: gating logic (clock enable, CS mux)

The register interface adds a few more chips for bus decode and the data register, but the core SPI engine is very compact.

## Simulation Model

### RTL: `sim_spi.sv`

The simulation SPI controller implements the same register interface as the real hardware. On the external side, it exposes signal-level ports to the testbench (same pattern as `sim_uart.sv`):

```systemverilog
module sim_spi (
    // Bus interface (same as sim_uart)
    input  logic        i_clk, i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we, i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // SPI signals exposed to testbench
    output logic        o_cmd_valid,   // Pulses when DATA is written
    output logic [7:0]  o_cmd_data,    // Byte being sent (MOSI)
    input  logic        i_resp_valid,  // Testbench presents response byte
    input  logic [7:0]  i_resp_data,   // Byte from testbench (MISO)
    output logic        o_cs0,         // Directly from CONTROL register
    output logic        o_cs1
);
```

The simulation model skips the actual shift register and clock divider — transfers complete in a configurable number of cycles (e.g., 2, like the UART TX busy simulation). This keeps the simulation fast while still exercising the polling loop.

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

This allows the full boot chain (ROM → stage 1 → stage 2 → kernel) to be tested in simulation with a real disk image.

## Address Map

The SPI controller's base address is assigned at boot by autoconfig. There is no fixed address — the ROM's autoconfig routine chooses where to place it based on device type and available address space.

## IRQ

Not initially needed — boot ROM and stage 1/2 use polling. The NetBSD driver will also start with polling. An IRQ line (optional, directly from DONE flag + enable bit) can be added later for interrupt-driven transfers or DMA.

## Design Decisions

1. **Byte-at-a-time transfers:** No FIFO or burst mode. Software polls STATUS and reads/writes DATA one byte at a time. This means 512 polling loops per SD sector, which is slow but simple. If performance becomes an issue, a DMA controller (separate bus master) is the likely solution rather than adding FIFOs to individual peripherals — this keeps each device simple and concentrates the complexity in one place.

2. **Autoconfig, not hardwired:** The SPI controller participates in the bus autoconfig protocol. Boards without SD cards simply omit the device. The boot ROM detects its presence (or absence) during enumeration.

3. **Simulation uses approximate latency:** The `sim_spi.sv` model completes transfers in a configurable number of cycles (no actual shift register or clock divider simulation). This keeps simulation fast while still exercising the polling loop and SD protocol state machine.

## Open Questions

1. **DMA:** Eventually we want DMA for bulk SD transfers (mentioned in boot-process.md). The SPI controller could be a DMA target, or DMA could be a separate bus master that reads the SPI data register. Deferred until the DMA controller design.

2. **CS pin count:** Two CS pins (SD + one expansion) should be enough. More can be added via a separate GPIO or decode chip.

3. **Real hardware clock:** The ECP5 runs at 25 MHz (or whatever we choose). SD operational speed of 12.5 MHz (CLKDIV=0) is fine. For higher speeds, we'd need a faster system clock or a dedicated SPI clock domain.

4. **Autoconfig device ID:** Need to define the Penumbra device ID space and assign an ID for the SPI controller.

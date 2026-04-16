# Penumbra -- TODO

Items needed for improved userland testing and interactive use.

## ISS Raw TTY Mode — DONE

Implemented: `+raw` flag, Ctrl-A escape prefix (X=exit, C=CPU
state, H=help), `make simulate RAW=1`.

## Boot Arguments — DONE

Implemented: bootloader reads `boot.cfg` from FAT32 via libsa
`perform_bootcfg()`, `root=psd0f` emits `BTINFO_ROOTDEVICE`,
kernel `cpu_rootconf()` auto-selects root device.
`make sdimage-rootfs` includes `boot.cfg` automatically.

## SPI FIFO + MI sdmmc

The current SPI controller is byte-at-a-time polled, making SD
card I/O very slow.  The current `psd` kernel driver is read-only
(CMD17 only) and custom (not using NetBSD's MI sdmmc stack).

Rather than building a complex MI sdmmc driver against the
current hardware interface and then reworking it, we settle the
hardware first.

### Interrupt Model

**PCI-style shared IRQ, no interrupt controller.**

One shared open-drain `/IRQ` line on the bus.  Any device can
assert it; the CPU's existing `i_irq` input (vector 9) is the
wire-OR of all device IRQ outputs.  There is no interrupt
controller module — identification is done in software by
polling each device's status register.

This is the simplest design for both FPGA and discrete 74xx:
one backplane wire, one pull-up, one open-collector gate per
device.  The same design works whether a device is "onboard"
(inside the FPGA) or external (discrete board on the bus) —
no architectural distinction between the two.

The UART already has IIR (16450 spec).  Each new device adds
an IRQ status register.  The kernel `EXC_EXT_IRQ` handler
walks known devices checking status.  Interrupt rate stays
low enough that polling cost is negligible.

### Phase 1: SPI Controller v2 — FIFO Extension

Extend `spi.sv` with a hardware TX/RX FIFO so the SPI engine
can shift bytes autonomously while the CPU does other work.

**Register interface** (existing 0x00–0x0C unchanged for
backward compatibility with ROM and bootloader):

| Offset | Name        | Mode | Description                          |
|--------|-------------|------|--------------------------------------|
| 0x00   | DATA        | R/W  | Single-byte TX/RX (existing)         |
| 0x04   | STATUS      | R    | BUSY, DONE (existing)                |
| 0x08   | CONTROL     | R/W  | CS, CPOL, CPHA (existing)            |
| 0x0C   | CLKDIV      | R/W  | Clock divider (existing)             |
| 0x10   | FIFO_DATA   | R/W  | FIFO TX/RX port (push/pop)           |
| 0x14   | FIFO_STATUS | R    | TX level, RX level, flags            |
| 0x18   | FIFO_CTRL   | W    | Transfer count, start, flush TX/RX   |
| 0x1C   | IRQ_STATUS  | R/W  | IRQ flags (write-1-to-clear)         |
| 0x20   | IRQ_ENABLE  | R/W  | IRQ mask (per-source enable)         |
| 0x24   | CAP         | R    | Version, FIFO TX depth, RX depth     |

**Key design points:**
- **Capability discovery:** CAP register reports FIFO depth.
  ROM/bootloader ignore it and use DATA register as before.
  Kernel reads CAP, adapts transfer strategy to FIFO size.
- **Adaptive FIFO depth:** Hardware-determined, software
  adapts.  On FPGA, 512+ bytes (one full sector without CPU
  interaction).  On discrete 74xx, maybe 16–32 bytes with
  watermark-driven refill.
- **IRQ sources** (bits in IRQ_STATUS/IRQ_ENABLE):
  - Transfer complete (FIFO_CTRL count reached zero)
  - RX FIFO above high-water mark (for small FIFOs)
  - TX FIFO below low-water mark (for small FIFOs)
- **o_irq output:** Active when any (IRQ_STATUS & IRQ_ENABLE)
  bit is set.  Directly wired into the shared `/IRQ` line.
- **Transfer count:** Write byte count to FIFO_CTRL + start
  bit.  Hardware clocks that many bytes from TX FIFO to SPI
  bus, captures RX into RX FIFO.  CPU fills TX FIFO before
  start (or in watermark-IRQ batches for small FIFOs).
- **Backward compatibility:** Single-byte DATA register
  (0x00) works exactly as before, ignoring FIFO entirely.
  CAP reads as version=0 on the old controller (unmapped
  register returns 0), so software can detect the upgrade.

**Deliverables:** RTL module + Verilator testbench.

### Phase 2: Kernel IRQ Dispatch + ISS Update

Wire the shared IRQ:
- `machine_sim.sv`: OR all device `o_irq` outputs into
  `i_irq` (replaces current `combined_irq`)
- `trap.c`: replace `EXC_EXT_IRQ` panic with polling loop
  that checks UART IIR, SPI IRQ_STATUS, etc.

Update ISS to emulate:
- SPI FIFO registers (CAP, FIFO_DATA, FIFO_CTRL, etc.)
- Multi-block SD commands (CMD18, CMD24, CMD25)
- Shared IRQ delivery
- Fix STATUS register bit mapping (BUSY vs DONE)

### Phase 3: MI sdmmc Kernel Driver

Implement `sdmmc_chip_functions` for the Penumbra SPI
controller (with FIFO support).  Attach NetBSD's MI
`sdmmc`/`ld_sdmmc` stack.  Remove custom `psd` driver.

**exec_command callback** translates `struct sdmmc_command`
into SPI byte sequences using the same protocol as psd
(command framing, R1/R3/R7 responses, data tokens).

**FIFO strategy** (adaptive based on CAP register):
- depth >= 512: fill TX FIFO with entire sector, start,
  wait for completion IRQ, drain RX FIFO
- depth < 512: watermark-driven streaming — fill to
  high-water, start, refill on TX-low IRQ, drain on
  RX-high IRQ

**Boot device naming change:** `psd0f` → `ld0f` (or similar).
Update `boot.cfg` and documentation.

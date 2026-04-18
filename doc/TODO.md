# Penumbra -- TODO

Items needed for improved userland testing and interactive use.

## ISS Raw TTY Mode — DONE

Implemented: `+raw` flag, Ctrl-A escape prefix (X=exit, C=CPU
state, H=help), `make simulate RAW=1`.

## Boot Arguments — DONE

Implemented: bootloader reads `boot.cfg` from FAT32 via libsa
`perform_bootcfg()`, `root=ld0f` emits `BTINFO_ROOTDEVICE`,
kernel `cpu_rootconf()` auto-selects root device.
`make sdimage-rootfs` includes `boot.cfg` automatically.

## SPI FIFO + MI sdmmc

The v1 SPI controller was byte-at-a-time polled, making SD card
I/O slow.  The original `psd` kernel driver was read-only (CMD17
only) and custom — not using NetBSD's MI sdmmc stack.

The roadmap settled the hardware first (SPI v2 with FIFO +
transfer engine + IRQ), then landed the shared-IRQ dispatcher,
then replaced `psd` with a proper MI sdmmc host (`pmci`) in
polled mode as a clean baseline.  Only the FIFO/IRQ-driven
`pmci` variant (Phase 3.5) remains.

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

### Phase 2: Kernel IRQ Dispatch — DONE

Implemented:
- `machine_sim.sv` and ISS already OR every device's `o_irq`
  into the CPU's `i_irq` input.
- `netbsd/sys/arch/penumbra/penumbra/intr.c` — shared-IRQ
  dispatcher with `LIST_HEAD` handler registry.  Public API:
  `intr_establish(_xname)`, `intr_disestablish`, `intr_init`.
  Walks every handler unconditionally on each IRQ (wire-OR
  semantics — short-circuiting would strand a simultaneous
  asserter).  Per-handler `struct evcnt` under group
  `"shared irq"` exposes counts to `vmstat -i`; separate
  spurious counter for unclaimed IRQs.
- `trap.c` EXC_EXT_IRQ calls `intr_dispatch()` with
  `ci_idepth++/--` bracketing.
- `com_pbbus.c` registers `comintr` as the first consumer
  (replaces the former `sc_poll_ticks=1` callout).

Note on ISS vs RTL TX IRQ counts: the ISS UART model reports
THRE≈instant, so `comintr`'s drain loop blasts through the
entire TX queue per IRQ.  Observed count ≈ number of write()
calls, not byte count.  RTL sim gives 1 IRQ/byte (TX_BUSY
simulated as 2170 cycles).  SPI FIFO on real hardware
coalesces similarly via watermark IRQs.

Deferred to Phase 3 (part of the sdmmc work):
- Multi-block SD commands (CMD18, CMD24, CMD25) in the ISS.

### Phase 3: MI sdmmc Kernel Driver — DONE (polled)

Implemented:
- `netbsd/sys/arch/penumbra/penumbra/pmci.c` — host
  controller driver, implements `sdmmc_chip_functions`
  over the SPI v2 register interface.  Polled
  byte-at-a-time transfers (FIFO_EN=0); FIFO-burst path
  deferred to Phase 3.5.
- `files.penumbra` pulls in `dev/sdmmc/files.sdmmc` and
  `kern/subr_disk_mbr.c` (for `readdisklabel`).
- `bus_dma_*` panic stubs in `bus_space.c` — the MI
  sdmmc code references them behind SMC_CAPS_DMA, which
  we never set.
- Custom `psd.c` driver removed.
- `boot.cfg` default changed to `root=ld0f` in
  `sw/tools/mksdimage.sh`.
- Boot ROM path, bootloader, and kernel all verified
  end-to-end via `make simulate SDCARD=...` — boots to
  single-user shell.

### Phase 3.5: SPI FIFO + IRQ-driven pmci

Extend `pmci_exec_command` to use the SPI v2 FIFO-burst
engine for the 512-byte data phase, with `intr_establish_xname()`
wakeups on XFER_DONE (large-FIFO) or TX_THRESH/RX_THRESH
(small-FIFO, discrete build).  Polled baseline remains
the fallback / reference implementation.

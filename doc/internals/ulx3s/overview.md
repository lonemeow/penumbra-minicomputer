# ULX3S

> **Applies to:** the Radiona ULX3S board (Lattice ECP5-85F) · its
> machine tops, timing probes and bring-up tops.

The ULX3S is the machine's first board and its reference: an ECP5-85F,
a 25 MHz crystal, 32 MiB of SDR SDRAM, a microSD slot, a GPDI (HDMI)
connector, a second USB port wired straight to the FPGA, a 3.5 mm jack
behind a resistor ladder, and an ESP32. Every core generation runs on
it, and it boots NetBSD with display, USB and audio. The CPU behaves
functionally the same on every board ([Nexys Video](../nexysvideo/overview.md)), and
what differs, software discovers: the machine's name ("ULX3S") and
clock from `SYSDEV_MACH`, the RAM by probing to a bus fault, and the
devices through autoconfig.

It builds with the open ECP5 flow (yosys, nextpnr, ecppack) and flashes
with openFPGALoader, all in containers (`DEVELOP.md`, "FPGA Synthesis
(ULX3S)"; `doc/internals/build-system.md`, "Device families").

## Tops

| Top | Build | What it is |
|-----|-------|------------|
| `ulx3s_penumbra1_top` | `make fpga BOARD=ulx3s CORE=penumbra1` | the gen1 machine |
| `ulx3s_penumbra2_top` | `make fpga BOARD=ulx3s CORE=penumbra2_5` (or `penumbra2`) | the gen2 machine |
| `ulx3s_penumbra2_probe_top` | `VARIANT=probe` | the bare gen2 machine against a block-RAM memory, for timing only |
| `ulx3s_penumbra3_probe_*_top` | registered per probe | gen3 timing probes |
| `ulx3s_video_test_top`, `ulx3s_audio_test_top` | `TOP=` | the display and audio paths alone |

Both machine tops carry a 64 KiB boot ROM, the UART, the SD slot over
SPI, the USB host controller, the display adapter and the audio
device, with SDRAM as system RAM. Each assembles the external bus
around its core, the device decode and the response fabric with its
watchdog, in the same shape the simulation wrappers present.

## Clocking and reset

One PLL (`EHXPLLL`) off the 25 MHz crystal makes four clocks, its
dividers derived from `CPU_HZ` and the SDRAM rate by
`ecp5_pll_compute`:

| Output | Rate | Drives |
|--------|------|--------|
| CLKOP | `CPU_HZ`: 25 MHz (gen1), 37.5 MHz (gen2) | the CPU and system bus |
| CLKOS | 100 MHz | the SDRAM controller |
| CLKOS2 | 100 MHz, phase-shifted | the SDRAM clock pin, forwarded through `ODDRX1F` |
| CLKOS3 | 60 MHz | the USB host controller |

The build stops (`build_check`) when no legal divider set reaches
`CPU_HZ`, when the SDRAM divider leaves the one the phase table
assumes, or when the VCO cannot also serve 60 MHz. A second PLL makes
the display's 25 MHz pixel and 125 MHz serial clocks, so video never
constrains the CPU clock. The CPU rates are fixed operating points,
where the fabric's timing runs out: gen2 closes 37.5 MHz only
narrowly.

The CLKOS2 phase is empirical, since boards and SDRAM parts see
slightly different data windows. `PHASE_DEG` (180 by default) sets it
at build time, and a sweep over the eight steps finds the arc that
boots; `doc/internals/sdram-controller.md`, "Step-5 phase sweep", has
the procedure.

Reset holds while the PLL is unlocked or FIRE1 (`btn[1]`) is pressed,
then for 2^19 CPU cycles. It reaches thousands of flip-flops, so a
`DCCA` puts it on a global net; the SDRAM and USB clock domains take
it through synchronizers of their own.

## LEDs

LED 0 is a heartbeat (bit 24 of a CPU-clock counter) and LED 1 the
PLL's lock; the other six are dark.

## Memory

32 MiB of 16-bit SDR SDRAM (a W9825G6KH or compatible) behind the
repository's own controller at 100 MHz, crossing from the CPU clock
through `sdram_cdc` ([SDRAM Controller v2](../sdram-controller.md)).
A top picks its stack as a fileset: the gen1 top the word stack, the
gen2 tops the line stack (`SDRAM_STACK_<top>` in the Makefile), a
choice measured per top like a placement seed. The PHY
(`sdram_phy_ecp5`) holds every SDRAM signal in I/O flip-flops.

## Devices and connectors

- **Console:** the one FTDI chip (FT231X) on US1 carries both the
  console and the programmer's JTAG, so flashing and the console share a
  cable. Its lines are named from the FTDI's side: `ftdi_rxd` is the
  FPGA's transmit line.
- **SD:** the slot is always powered and has no card-detect line. SPI
  uses the clock, command, `sd_d[0]` (MISO) and `sd_d[3]` (chip
  select), with `sd_d[1]` and `sd_d[2]` held high.
- **USB host:** US2's data lines and pull resistors are FPGA pins, so
  the host controller drives the wire itself, at low and full speed
  (`doc/internals/usb-host-controller.md`).
- **Display:** GPDI, HDMI-compatible, TMDS from `ODDRX1F` pairs
  (`doc/internals/display.md`).
- **Audio:** a four-bit resistor ladder per channel into the 3.5 mm
  jack, driven by the audio device's sigma-delta output.
- **ESP32:** held in reset (`wifi_en` low).

## Timing and placement

The ECP5 runs out of routing long before logic: a machine top uses
about 30% of the LUTs and still closes timing only narrowly. A change
is judged by the fabric it adds, memories belong in block RAM, and near
the limit a cut in logic depth comes back as only about half as much
fmax, routing taking the rest.

nextpnr's result depends on its placement seed, so each default top
pins one (`DEFAULT_SEED_<top>`): the seed with the widest worst margin
across all five clock domains in a `hw/tools/seed-sweep.sh` run. A
seed belongs to one netlist and one set of clock targets; any change
to either invalidates it.

Constraints come in two kinds that never mix. The vendor board file
(`hw/constraints/ulx3s_v20.lpf`) holds every pin and I/O standard and
is never edited. A top may add design intent on top of it: an LPF
overlay of regions and timing targets, and a floorplan script nextpnr
runs before packing, since its LPF cannot express regions. The gen2
top has both, to keep the SDRAM controller compact in the narrow
corridor its core leaves beside the SDRAM pins.

The ECP5 flow has no signoff gate: nextpnr reports its fmax, and a
build that misses it still packs (`doc/TODO/hw-ulx3s.md`). Clock
crossings get the gate-level check the flow lacks: `make test-gate`
simulates the synthesized `sdram_cdc` with a routing delay on every
cell input, which is where a crossing's glitch hazards show.

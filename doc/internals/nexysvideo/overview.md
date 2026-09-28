# Nexys Video

> **Applies to:** the Digilent Nexys Video board (XC7A200T-1SBG484C) ·
> its machine top and bring-up tops. The memory system has its own
> document, [ddr3.md](ddr3.md).

The Nexys Video is the machine's second board: an Artix-7 200T, a
100 MHz oscillator, 512 MiB of DDR3, a microSD slot and a USB UART, plus
HDMI, Gigabit Ethernet, an audio codec, an OLED and Pmod headers the
machine does not use yet (`doc/TODO/hw-nexysvideo.md`). It runs the
gen2 machine from the same boot ROM and NetBSD as the ULX3S. The CPU
behaves functionally the same on both boards, and what differs,
software discovers: the machine's name and clock from `SYSDEV_MACH`,
the RAM by probing to a bus fault, and the devices through
autoconfig.

It builds with Vivado, in a container (`DEVELOP.md`, "Vivado (Xilinx
boards)"; `doc/internals/build-system.md`, "Device families"), and
every build passes its signoff gates before it can be flashed.

## Tops

| Top | Build | What it is |
|-----|-------|------------|
| `nexysvideo_penumbra2_top` | `make fpga BOARD=nexysvideo CORE=penumbra2_5` (or `penumbra2`) | the machine |
| `nexysvideo_uart_test_top` | `make fpga TOP=nexysvideo_uart_test_top` | the console alone: a banner, an echo, and LEDs for lock, reset, a heartbeat, RX activity and framing errors |
| `nexysvideo_ddr3_test_top` | `make fpga TOP=nexysvideo_ddr3_test_top` | the DDR3 controller and its whole-chip self-test, no CPU |

The machine top carries a 64 KiB boot ROM, the UART, the microSD slot
over SPI behind autoconfig, and system RAM in DDR3, and names itself
"Nexys Video". It assembles the external bus around the machine as
the ULX3S top does: the device decode, and the response fabric with
its 256-cycle watchdog.

## Clocking and reset

The CPU clock comes from the 100 MHz oscillator through an MMCM
(`nexysvideo_clk_rst`, shared by every top). `xc7_mmcm_pkg` derives
the dividers from `CPU_HZ`, so the clock is a one-number edit, and the
build stops when no exact divider set reaches it. The machine top runs
at 60 MHz. The DDR3 controller has clocks of its own, from a PLL
([ddr3.md](ddr3.md), "Clocks, the crossing and reset").

The reset holds while the MMCM is unlocked or CPU RESET is pressed,
and for at least 10 ms after both clear, the bus's reset minimum
([bus protocol](../../hardware/bus-protocol.md)); a press or a loss of
lock restarts the hold, so a bouncing button does too. The MMCM's own
reset is tied off, so its lock reports the MMCM alone. CPU RESET
(`cpu_resetn`) is active-low and sits in the DDR3 bank at 1.5 V. The
machine top also holds the machine in reset until the DDR3 has
calibrated.

## LEDs

The machine top's LEDs report what the machine cannot print:

| LED | Lit when |
|-----|----------|
| 0 | the MMCM is locked |
| 1 | reset is released |
| 2 | a 1 Hz heartbeat, counted off the CPU clock |
| 3 | the DDR3 has calibrated |
| 4 | a RAM access has waited at least an eighth of the bus watchdog (32 cycles) |
| 5 | ... at least a quarter (64 cycles) |
| 6 | ... at least half (128 cycles) |
| 7 | the die is above the thermal warning threshold |

LEDs 4 to 6 hold the longest RAM wait since configuration
(`bus_wait_meter`), so they read the margin against the watchdog
after any workload; a reset does not clear them. The first four
localize a machine that never prints: a dark LED 0 is the MMCM, a dark
LED 3 the memory's calibration (which also keeps LED 1 dark), a dark
LED 1 with LED 3 lit the reset, and a wrong heartbeat rate the clock.

## Console and SD

The console is the board's UART port, an FT232R of its own, separate
from the PROG port's JTAG, at 115200 8N1. The master XDC names its
lines from the host's side: `uart_rx_out` is the FPGA's transmit line
and `uart_tx_in` its receive line.

The microSD slot is shared with the board's PIC24, which holds the
card until the FPGA is configured, then switches the slot's power off
and releases the lines. The top drives the power switch (`sd_reset`,
low powers the card) from the machine's reset, so the card is
unpowered while the machine is held in reset. The card's data-out
line (`sd_d[0]`, MISO) answers a clock edge the CPU clock launched,
and `spi.sv` samples it two CPU cycles later; the constraints state
its worst arrival, 25 ns after the edge (clock-to-out, the board's
flight both ways and the card's 14 ns output delay), as a two-cycle
path. The slot's card-detect switch is not used.

## I/O standards and constraints

The board's banks run at four voltages, and a top copies each pin's
I/O standard from Digilent's master XDC unchanged: LVCMOS12 for the
buttons and switches (the VADJ bank; with `set_vadj` and `vadj_en`
undriven, the board's pull resistors set it to 1.2 V), LVCMOS15 for CPU
RESET (the DDR3 bank), LVCMOS25 for the LEDs and Ethernet, LVCMOS33
for the UART, SD and Pmods.

A top's constraints can span several files:

- `hw/constraints/nexysvideo_machine.xdc`: the board lines every
  machine top uses, whatever its core, and the design's timing
  intent (false paths on the asynchronous pins, the SD input timing,
  the bound on the CPU/DDR3 crossing);
- `hw/constraints/nexysvideo_ddr3.xdc`: the DDR3 pins, taken from
  Digilent's memory-interface project, shared by every top with the
  memory;
- one file per bring-up top.

`hw/tools/xdc_check.py` checks a top's files as a set against the
vendored masters (`hw/third_party/digilent-xdc/`,
`hw/third_party/digilent-boards/`): every pin location, I/O standard
and oscillator clock must be one of the masters' lines, unchanged, and
every board-wide setting must be present. Everything else, timing
exceptions and crossing constraints included, is design intent and
passes unchecked.

## Thermal guard

The board has no fan. Every top on a 7-series part arms the XADC's
over-temperature shutdown (`hw/rtl/fpga/xc7/xc7_thermal_guard.sv`),
and the xc7 flow refuses a design without it. Past 85 °C, the
commercial-grade part's junction limit, the device disables itself
and releases every pin, with no logic of the design involved, so it
acts on a hung design too.

A shut-down device would start again by itself once it cooled, from
the state it stopped in, with its DRAM unrefreshed. The guard sets the
release threshold below any temperature the board sees, so the device
stays off until it is reprogrammed or its power is removed. A warning
lights LED 7 from 75 °C until the die cools below 70 °C. The XADC runs
off the oscillator, so the guard keeps measuring whatever the design's
clocks and reset are doing. Under the DDR3 self-test running without
pause, the die settled at 39 °C from 35 °C cold.

What a shut-down board looks like, and how to read the die temperature
over JTAG, is in `DEVELOP.md`, "Thermal protection". A sensor device
that shows the temperature to the kernel is open work
(`doc/TODO/hw-devices.md`, "Hardware + kernel: die temperature and
thermal warning for the kernel").

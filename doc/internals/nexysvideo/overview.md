# Nexys Video

> **Applies to:** the Digilent Nexys Video board (XC7A200T-1SBG484C) ·
> its machine tops and bring-up tops.

The Nexys Video is the machine's second board: an Artix-7 200T, a
100 MHz oscillator, 512 MiB of DDR3, a microSD slot, a USB UART, an
HDMI output, an audio codec, Gigabit Ethernet and a USB HID socket
for a keyboard, plus an HDMI input, an OLED and Pmod headers the
machine does not use yet (`doc/TODO/hw-nexysvideo.md`). It runs the
gen2 and gen3 machines from the same boot ROM and NetBSD as the ULX3S.
The CPU behaves functionally the same on both boards, and what
differs, software discovers: the machine's name and clock from `SYSDEV_MACH`,
the RAM by probing to a bus fault, and the devices through
autoconfig.

It builds with Vivado, in a container (`DEVELOP.md`, "Vivado (Xilinx
boards)"; `doc/internals/build-system.md`, "Device families"), and
every build passes its signoff gates before it can be flashed.

The board's larger subsystems have documents of their own:

- [`ddr3.md`](ddr3.md) — the memory system: the vendored DDR3
  controller, the glue between it and the bus, and its clocks and
  latency.
- [`audio.md`](audio.md) — the audio path: the device in front of the
  codec, the codec's setup over I2C and the clocks between them.
- [`ethernet.md`](ethernet.md) — the Ethernet adapter: the PHY, its
  clocks and reset, and where the station address comes from.

## Tops

| Top | Build | What it is |
|-----|-------|------------|
| `nexysvideo_penumbra2_top` | `make fpga BOARD=nexysvideo CORE=penumbra2_5` (or `penumbra2`) | the gen2 machine |
| `nexysvideo_penumbra3_top` | `make fpga BOARD=nexysvideo CORE=penumbra3` | the gen3 machine, its caches the ULX3S's |
| `nexysvideo_uart_test_top` | `make fpga TOP=nexysvideo_uart_test_top` | the console alone: a banner, an echo, and LEDs for lock, reset, a heartbeat, RX activity and framing errors |
| `nexysvideo_ddr3_test_top` | `make fpga TOP=nexysvideo_ddr3_test_top` | the DDR3 controller and its whole-chip self-test, no CPU |

The two machine tops differ only in the machine; the board around it,
which the rest of this document and the subsystem documents describe,
is the same. Each carries a 64 KiB boot ROM, the UART and system RAM in
DDR3, and names itself "Nexys Video". Behind autoconfig it carries
the microSD slot over SPI, the display adapter on the HDMI output, the
audio device in front of the codec ([audio.md](audio.md)), the
Ethernet adapter on the board's PHY ([ethernet.md](ethernet.md)) and
the PS/2 port on the keyboard bridge. It assembles the external bus
around the machine as the ULX3S top does: the device decode, and the
response fabric with its 256-cycle watchdog.

## Clocking and reset

The CPU clock comes from the 100 MHz oscillator through an MMCM
(`nexysvideo_clk_rst`, shared by every top). `xc7_mmcm_pkg` derives
the dividers from `CPU_HZ`, so the clock is a one-number edit, and the
build stops when no exact divider set reaches it. The machine tops run
at 60 MHz. The DDR3 controller has clocks of its own, from a PLL
([ddr3.md](ddr3.md), "Clocks, the crossing and reset").

The reset holds while the MMCM is unlocked or CPU RESET is pressed,
and for at least 10 ms after both clear, the bus's reset minimum
([bus protocol](../../hardware/bus-protocol.md)); a press or a loss of
lock restarts the hold, so a bouncing button does too. The MMCM's own
reset is tied off, so its lock reports the MMCM alone. CPU RESET
(`cpu_resetn`) is active-low and sits in the DDR3 bank at 1.5 V. The
machine tops also hold the machine in reset until the DDR3 has
calibrated and the Ethernet PHY is past its own reset
([ethernet.md](ethernet.md), "Reset").

The display has an MMCM of its own (`nexysvideo_video_clk`): 25 MHz
for the pixel clock and 125 MHz for the TMDS serial clock, both off a
1000 MHz VCO, the one frequency in the MMCM's band that also divides
to the second display mode's 40 and 200 MHz. Its output chain resets
while the machine does or while that MMCM is unlocked. The reset is
formed in the pixel domain, so the serial domain, related to it at
5x, takes it on a timed path.

The codec's master clock, 12.5 MHz, is the oscillator divided by eight
in a counter (`nexysvideo_codec_mclk`), with no clock manager: it runs
from configuration on and no reset stops it ([audio.md](audio.md),
"Three clocks, and who drives each").

The Ethernet adapter's transmit side runs on the oscillator itself and
its receive side on the clock the PHY sends, with no clock manager for
either ([ethernet.md](ethernet.md), "Clocks").

All three clock managers take the oscillator straight from its pin
(R4, in bank 34 and clock region X1Y2), and that region's clock
management tile holds one MMCM and one PLL. The CPU's MMCM and the
DDR3's PLL fill it, and Vivado places the display's MMCM in the tile
one region down (X1Y1), which the pin reaches over the tiles' vertical
backbone. It does so by itself: the constraints set no
`CLOCK_DEDICATED_ROUTE`, and placement raises no warning. Where a
further clock manager on the oscillator fits is its build's to find
out; one fed through a global buffer (`BUFG`) instead can take the
tile of any clock region.

## LEDs

A machine top's LEDs report what the machine cannot print:

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

## Keyboard

The USB HID host socket (J15) is connected to the board's PIC24
instead of the FPGA, and the PIC24 presents the keyboard or mouse
plugged in there as a PS/2 device on two FPGA pins, `ps2_clk` and
`ps2_data`. The machine puts its PS/2 port
([`../ps2-port.md`](../ps2-port.md)) on them, last in the autoconfig
chain, so the board has a keyboard without a USB host of its own.

Nothing on the board pulls the two lines up. The manual has a design
enable the FPGA's own pull-ups on both pins, which the constraints do,
and the top drives each pad open drain. The bridge sends a device's
self-test result (`0xAA`) when one is plugged in.

The manual describes the bridge only in outline. What it does beyond
that, measured with `benchmark/ps2test` and a low-speed USB keyboard:

- It is on the link with or without a keyboard. It clocks in and
  acknowledges every byte the port sends, so `TX_DATA.ERR` never
  shows an empty socket here: a command going unanswered does.
- With a keyboard it answers echo, read ID, set LEDs, enable and
  reset. An answer starts about 0.8 ms after the byte is taken, 13 ms
  at the longest seen, and the self-test result follows a reset by
  40 to 300 ms.
- About one byte in ten that the port sends is acknowledged and then
  lost: no action and no answer. Each byte fares independently of the
  one before, whatever its value, and the share drifts between a few
  percent and a quarter over seconds. When the first byte of a
  two-byte command is the one lost, the bridge takes the second for a
  command and answers `0xFE`. Why it loses them is open
  (`doc/TODO/hw-devices.md`, "Hardware + kernel: PS/2 port
  (`CLASS_PS2`)"). A driver sends a byte again when nothing answers
  it inside the protocol's 20 ms, and that is enough: of 1240 bytes
  of enable, LED and reset commands sent so, with four tries each,
  none failed.
- Asked with `0xFE`, it sends its last byte again. A kernel that took
  every sixteenth byte received for a damaged one asked 43 times
  while a keyboard was typed on: no request ran out of tries, and
  what was typed arrived whole.
- Held off, it keeps sixteen bytes. Past that it drops what arrives,
  keeping the newest in the last place, and reports no overrun.
- What it kept survives a reset of the machine, which does not reset
  the bridge.
- Not every keyboard is served. A low-speed one is; a Keychron C1 is
  not, and the bridge has no hub support to put one behind.

## Display

The display adapter (`CLASS_DISPLAY`, [display.md](../display.md))
sends DVI to the HDMI output: 640x480 at 60 Hz, from a 25 MHz pixel
clock 0.7% under VESA's, which monitors lock to. Each of the four TMDS
pairs leaves through an `ODDR` in its I/O cell into an `OBUFDS` at
`TMDS_33`, a current-mode driver that the monitor's termination to
3.3 V completes; with no monitor attached the pairs carry nothing. The
connector's hot-plug detect, DDC and CEC lines are unused.

Every clock manager on the board runs off the one oscillator, so
Vivado treats the CPU and pixel clocks as related and would time the
display's crossing between them at their closest edges. The crossing
is quasi-static control state through synchronizers, and memories in
dual-clock block RAM, so the constraints bound it the way they bound
the DDR3's, to one period of the faster clock.

## I/O standards and constraints

The board's banks run at four voltages, and a top copies each pin's
I/O standard from Digilent's master XDC unchanged: LVCMOS12 for the
buttons and switches (the VADJ bank; with `set_vadj` and `vadj_en`
undriven, the board's pull resistors set it to 1.2 V), LVCMOS15 for CPU
RESET (the DDR3 bank), LVCMOS25 for the LEDs and Ethernet, LVCMOS33
for the UART, SD, audio codec, I2C, PS/2 lines and Pmods, and TMDS_33
for the HDMI output's pairs.

A top's constraints can span several files:

- `hw/constraints/nexysvideo_machine.xdc`: the board lines every
  machine top uses, whatever its core, and the design's timing
  intent (false paths on the asynchronous pins and the HDMI pairs,
  the SD and Ethernet input timing, the bounds on the CPU clock's
  crossings to the DDR3, the pixel clock and the Ethernet clocks, the
  pull-downs on the codec's serial clocks);
- `hw/constraints/nexysvideo_ddr3.xdc`: the DDR3 pins, taken from
  Digilent's memory-interface project, shared by every top with the
  memory;
- one file per bring-up top.

`hw/tools/xdc_check.py` checks a top's files as a set against the
vendored masters (`hw/third_party/digilent-xdc/`,
`hw/third_party/digilent-boards/`): every pin location, I/O standard
and oscillator clock must be one of the masters' lines, unchanged, and
every board-wide setting must be present. A clock that a part on the
board sends, the Ethernet PHY's receive clock, is not the masters' to
state: the top gives its period from the part's datasheet. Everything
else, timing exceptions and crossing constraints included, is design
intent and passes unchecked.

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

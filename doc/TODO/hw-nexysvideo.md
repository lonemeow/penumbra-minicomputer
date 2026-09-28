# Hardware: Nexys Video board

Work bound to the Nexys Video board.

## Build system: Vivado thread count

Vivado caps itself at 8 threads (`general.maxThreads`), while its
container sees every host core (16 on the development machine). On
the Nexys Video gen2.5 machine top, synthesis is effectively serial
(3:08 of CPU over 3:10 elapsed), so more threads could only speed up
place, route and the analysis steps. Open: time a build of the DDR3
machine top at 8 threads against one at 16; if place and route shrink
noticeably, pass a make variable through to
`hw/tools/vivado/build.tcl`, kept adjustable so a concurrent LLVM
build still gets its share of the cores.

## Board workflow: the bitstream is lost at every power cycle

The FPGA forgets its configuration when the board loses power, so it
needs a `make flash` before it runs again, and it stays dark after an
unattended power cut. The board's QSPI configuration flash can hold a
bitstream the Artix-7 loads by itself at power-on, with the mode
jumper set to QSPI. Open: openFPGALoader writes that flash (`-f`), and
the constraints need the SPI configuration properties
(`BITSTREAM.CONFIG.SPI_BUSWIDTH`, `CONFIGRATE`) for a fast load; a
separate `make flash-persistent` target keeps the JTAG load as the
development default. The ULX3S has the same gap (`hw-ulx3s.md`,
"Board workflow: flashing, reset, and persistence annoyances").

## Devices: no display

The display adapter (`CLASS_DISPLAY`) drives the ULX3S's GPDI port;
the Nexys Video has an HDMI TX connector, and its machine has no
display. The adapter's pixel side is board-neutral, so what the
board needs is its output stage, the counterpart of the ULX3S's
`ODDRX1F` pairs: an `ODDR` and an `OBUFDS` per TMDS lane at 125 MHz
(TMDS_33), and 25 and 125 MHz from an MMCM of their own, as the
ULX3S gives video its own PLL, so the pixel clocks never constrain
the CPU clock.

## Devices: no USB host, so no keyboard, network or USB storage

On the ULX3S the USB host controller (`CLASS_USBHC`) carries the
keyboard, USB storage and the `ure(4)` Ethernet adapter that gives the
machine its network. The Nexys Video has no USB port the FPGA drives:
its USB host socket belongs to the PIC24, which presents a keyboard or
mouse as PS/2 (`ps2_clk`, `ps2_data`). Options:

- **USB host on a Pmod:** a USB-A socket with the host's pull-downs and
  series resistors, driven by the same `usbhc` and PHY
  (`usb_phy_ecp5` is plain RTL behind the top's pin buffers, its name
  notwithstanding) from a 60 MHz clock. Everything the ULX3S does over
  USB then works unchanged, the network included.
- **The board's own parts:** a PS/2 device for the PIC24's keyboard
  bridge, and an RGMII MAC for the RTL8211E Ethernet PHY. Each is a new
  device class and driver, and needs no Pmod hardware.
- **Network only:** the ESP32 NIC (`doc/system/devices/esp32-nic.md`)
  on a Pmod UART, with its existing firmware and `pnicctl`.

Open: which. The Pmod gives the most for the least new RTL.

## Devices: no audio

The audio device (`CLASS_AUDIO`) plays through a resistor ladder on
the ULX3S; the Nexys Video's audio is an ADAU1761 codec, which takes
I2S and needs its registers set over I2C before it plays anything.
The device's programming interface stays as it is: a Nexys output
stage serializes the samples to I2S, and the top sets the codec up
over I2C by itself after reset, so software sees the same
`CLASS_AUDIO` on both boards. Open: the output stage, the codec's
master clock, and its I2C setup sequence.

## Clocking: CPU clock above 60 MHz

The machine runs at 60 MHz. The DDR3 machine top's signoff
(2026-09-28) met timing with 1.697 ns of slack, an fmax of 66.8 MHz.
Its worst paths start at EX's `o_fault_pending` and end at MEM's
`o_wb_value` (14 levels, 79% route: RDSYS reading the main TLB back
out of its LUT RAM) or at the MMU's fault-status clear (13 levels).
The DDR3 controller's own clock is not a factor (83.33 MHz, fmax
98.1 MHz). Open: the next step up. DDR3's latency is fixed in
nanoseconds, so it costs more CPU cycles as the clock rises: re-read
the RAM wait LEDs at each step against the 256-cycle bus watchdog.

## Ruled out

- **Co-simulating the real DDR3 controller as routine verification.**
  UberDDR3 with the Micron DDR3 model simulates at about 1.4 s of wall
  time per simulated microsecond (upstream's xsim log for this
  configuration: 337 us in 7 min 50 s), so a directed run takes
  minutes and anything memtest-sized weeks. The self-test top covered
  calibration and data on the board instead (67 runs of BIST mode 2
  over the whole chip, byte masks included, no mismatches), and the
  stack's Wishbone side is tested against a model
  (`tb_ddr3_stack_test`, `make test SIM_MEM=ddr3`). Upstream's Icarus
  testbench stays the tool if the board ever disagrees with the model.
- **A status panel on the OLED that needs no CPU.** Planned for the
  case where DDR3 never calibrates and nothing prints; not built,
  since the LEDs and the self-test top's console line localized every
  bring-up failure.
- **openXC7 for the Xilinx flow.** nextpnr-xilinx does no static timing
  analysis, and no bitstream is flashed without signoff; Vivado runs
  in a container instead (`DEVELOP.md`, "Vivado (Xilinx boards)").

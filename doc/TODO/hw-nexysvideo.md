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

## Devices: the framebuffer at the display's full resolution

The display's framebuffer is 320x240 at 8 bpp, pixel-doubled onto the
640x480 raster, because that is what the ULX3S's block RAM holds (42
of its 208 EBRs; `hw-devices.md`, "Hardware + kernel: local console
(HDMI display + USB keyboard)"). The Nexys Video carries the same
device, and its XC7A200T has room for more. The machine with the
display uses 70 of its 365 block RAM tiles (the build of d507a6711f6b),
32 of them the framebuffer: eight a byte bank, where a bank's 19,200
bytes would fit in five at 4 KiB each, so Vivado rounds its depth up
as the ULX3S flow does. A framebuffer at the raster's own 640x480 at
8 bpp is 300 KiB, 76,800 bytes a bank: 19 blocks a bank at the ideal
packing, 76 in all, and about 96 at the packing this build shows,
which brings the machine to about 134 of the 365.

The class needs nothing new: `FB_GEOM` is read from the device, and
`pdisplay` sizes its framebuffer and its `mmap` from it. The device
does:

- storage for 300 KiB, and a device window of 512 KiB in place of
  256 KiB, since the pixel aperture at `0x20000` then ends at
  `0x6B000`;
- a scan-out that walks each line once, with no doubling: a word
  serves four pixel clocks instead of eight, and no line is replayed;
- the ULX3S keeping its 320x240 device, so the two boards' displays
  differ in geometry, which the contract allows and software reads.

The same 300 KiB holds 320x240 at 32 bpp (`hw-devices.md`,
"Hardware + kernel: direct-color framebuffer format"), so whether the
Nexys device offers one of the two, or both over one store, is
decided with that entry.

The cost is on the software side. A full-frame redraw writes four
times the bytes it does now, and framebuffer stores already cost:
Doom's 64 KB blit is 5% of a frame (`workloads.md`, "Doom: where a
frame goes"), and the stores' cache attribute is itself open
(`kernel.md`, "Kernel: `bus_space_mmap()` maps device memory
cacheable"). The consumers adapt without changes but gain little:
Doom copies its 320x200 frame row by row into whatever stride
`GET_FBINFO` reports, so it would fill a quarter of the screen
unless its blit doubles pixels, and the demos size their work from
the geometry, so each frame costs them four times the pixels. Open:
the storage and scan-out, and a consumer that wants the resolution.

## Devices: no USB host, so no USB storage

On the ULX3S the USB host controller (`CLASS_USBHC`) carries the
keyboard and USB storage. The Nexys Video has no USB port the FPGA
drives: its USB host socket belongs to the PIC24, which presents a
keyboard or mouse as PS/2. The machine's PS/2 port takes the keyboard
from there (`6b0ce9109d16`; `doc/internals/nexysvideo/overview.md`,
"Keyboard"), so what waits for a USB host is storage, and a keyboard
the bridge does not serve.

USB comes to the board on an FPGA Mezzanine Card made for it,
fmc-usb2 (a repository of its own): two USB 2.0 ports on USB3320
ULPI PHYs, which recover the receive clock, code the line and drive
it in silicon, where the ULX3S does all three in fabric. The card is
in design and will take a while. The machine's side of it:

- **The PHY:** `usb_phy_ulpi`, the controller's planned ULPI PHY
  (`doc/internals/usb-host-controller.md`), behind the existing
  UTMI-shaped seam, with the controller above it unchanged. Full
  speed comes up first, on the controller and driver the ULX3S runs,
  so a failure points at the card or the timing rather than the
  protocol; high speed follows (`hw-devices.md`, "Hardware: USB high
  speed").
- **VADJ:** the PHYs' I/O supply follows it, so the top drives it to
  1.8 V or above through `set_vadj` and `vadj_en`. Banks 15 and 16
  carry the FMC's LA pins and also the buttons and switches, which
  the master XDC gives LVCMOS12. The machine top uses none of them; a
  top that did would need `xdc_check.py` to accept the I/O standard
  the bank's VADJ implies.

Neither the keyboard nor the network waits for the card
(`hw-devices.md`, "Hardware + kernel: PS/2 port (`CLASS_PS2`)" and
"Hardware + kernel: Ethernet MAC for an onboard PHY").

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
- **USB host on a Pmod, through the ULX3S's fabric PHY.** A USB-A
  socket on a Pmod, driven by `usb_phy_ecp5` as the ULX3S drives its
  port. On the ULX3S that path is unreliable at full speed
  (`hw-devices.md`, "Hardware: USB full speed unreliable on the
  ULX3S"), with the fabric's receive clock recovery a leading suspect;
  a PHY chip takes that into silicon, and fmc-usb2 carries two.

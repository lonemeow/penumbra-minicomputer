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

## Devices: no USB host, so no keyboard or USB storage

On the ULX3S the USB host controller (`CLASS_USBHC`) carries the
keyboard and USB storage. The Nexys Video has no USB port the FPGA
drives: its USB host socket belongs to the PIC24, which presents a
keyboard or mouse as PS/2 (`ps2_clk`, `ps2_data`).

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

Until the card, the machine has no keyboard: the only one that needs
no card is a PS/2 device behind the PIC24's bridge. The network does
not wait for USB (`hw-devices.md`, "Hardware + kernel: Ethernet MAC
for an onboard PHY").

## Devices: no audio

The audio device (`CLASS_AUDIO`) plays through a resistor ladder on
the ULX3S. The Nexys Video's four jacks all belong to an ADAU1761
codec (IC4), which takes I2S and needs its registers set over I2C
before it plays anything. Software sees the same class on both
boards: the top sets the codec up by itself, and NetBSD plays through
`/dev/audio` with the kernel and image the ULX3S runs. The device
already reports the setup's progress and failure in `STATUS`
(`doc/system/devices/audio.md`, "STATUS (0x020)"), and `paudio` reads
it at attach.

### The board

From the schematic (sheet 4, rev A.0):

- **Line out** (green, J5): LOUTP and ROUTP through 10 µF and 100 Ω,
  with 10 kΩ to ground on the jack side; LOUTN and ROUTN are not
  connected.
- **Headphones** (black; J4 on the schematic, J3 in the reference
  manual): LHP and RHP through 220 µF, the sleeve to ground. The
  capless option is not fitted (R102 from MONOOUT to the sleeve, and
  R103 and R105 across the capacitors, are all unloaded), so MONOOUT
  drives nothing.
- **Inputs**: line in (blue) to LAUX and RAUX, the microphone (pink)
  to LINN and RINN with mic bias. Playback uses neither.
- **Control**: I2C at 0x3B (ADR0 and ADR1 pulled up), 4.7 kΩ pull-ups
  to 3.3 V, on a bus shared with the MAC-address EEPROM
  (`hw-devices.md`, "Hardware + kernel: Ethernet MAC for an onboard
  PHY"). MCLK comes straight from the FPGA. No jack detect (JDET is
  pulled down).
- **Supplies**: IOVDD at 3.3 V, and AVDD at 3.3 V from a regulator of
  its own, so DAC full scale is 920 mV rms.
- **No reset pin**: the codec keeps its registers across FPGA reloads
  and CPU resets, from board power-on until the board loses power.

The pins are `ac_mclk` (U6), `ac_bclk` (T5), `ac_lrclk` (U5),
`ac_dac_sdata` (W6), `ac_adc_sdata` (T4, unused by playback), `scl`
(W5) and `sda` (V5).

### The load: amplified speakers on the line out

External amplified speakers are the case that has to be right, and
the line out is built for them. Into a 10 kΩ amplifier its coupling
corner is about 2.6 Hz and it settles in about 60 ms, and the bleed
resistors hold the jack at 0 V with nothing plugged in.

The headphone jack's 220 µF is sized for 16 to 32 Ω (a 45 Hz corner at
16 Ω), and into 10 kΩ it is a 2.2 s time constant: any move of the
codec's 1.65 V output bias reaches an amplifier as a thump lasting
seconds. With nothing plugged in, the jack side has no path to ground
and rises with the bias, so it pops into whatever is plugged in next.
Amplifiers go on the line out; the headphone jack is driven as well
because it costs two register writes.

### The design

- **The codec is the clock master.** Its PLL makes the 49.152 MHz core
  clock (1024 × 48 kHz) from any MCLK in 8 to 27 MHz, and the codec
  drives BCLK (64 × fs, 3.072 MHz) and LRCLK; the FPGA takes its frame
  timing from LRCLK, so the sample rate depends on no FPGA clock. The
  FPGA as master would need MCLK locked to its frame tick, which no
  clock on the board gives: the display MMCM's 1000 MHz VCO has no
  factor of 3, and the CPU clock is a one-number edit.
- **MCLK** is 12.5 MHz from a PLL of its own (a 1000 MHz VCO ÷ 80),
  forwarded through an `ODDR`. The codec's PLL multiplies it by
  3 + 2913/3125, exactly 49.152 MHz. The setup's PLL words depend on
  the MCLK frequency, so MCLK comes from a clock nothing else retunes.
  The X1Y1 clock tile's PLL is free
  (`doc/internals/nexysvideo/overview.md`, "Clocking and reset").
- **The datapath**: R58 `SINRT = 0001` routes the serial input straight
  to the DACs, so the SigmaDSP stays off and needs no program. The DACs
  feed mixers 3 and 4; those drive the headphone outputs in headphone
  mode, and mixers 5 and 6 into LOUTP and ROUTP in line mode. Mixer 7
  and the mono output stay off.
- **Levels** are fixed at setup, since `MASTER_VOL` gives software its
  volume: the line out at 0 dB (920 mV rms full scale, under the 2 V
  rms a line input takes), the headphones at −10 dB.
- **Pops**: once biased, the playback path is never powered down; the
  setup mutes and unmutes instead, waiting the datasheet's 4 ms or more
  between enabling the headphone outputs and unmuting them. That leaves
  at worst one thump per board power cycle, when the bias first comes
  up. Whether it comes up at the codec's power-on or when the playback
  path is enabled, the datasheet does not say.
- **The setup runs at every machine reset** and writes every register
  it depends on, never relying on a default: the codec may still be
  playing what an earlier bitstream set up.

### Steps

The device side landed first: it takes its frame timing from the
output stage (e358bfc11c62), and reports readiness and a fault code
in `STATUS` ("hw: report the audio device's readiness in STATUS").

1. **I2C and the codec setup.** A generic I2C master (start, repeated
   start, a byte written with its ACK checked, a byte read with ACK or
   NACK, stop), since the EEPROM shares the bus. Over it, the setup
   sequencer:
   1. R0 and R1: the PLL off, its six bytes in one write, the lock
      bit polled against a timeout, then `COREN`;
   2. every output muted;
   3. the serial port as I2S master at 64 BCLKs a frame, the routing,
      the clock enables, and the DSP off;
   4. the DACs, playback power and the mixers;
   5. the precharge wait, then the unmute.

   It drives `STATUS.BUSY` and `STATUS.FAULT` (a NACK, or no lock),
   with a `FAULT_CODE` naming the step that failed and how, and
   finishes inside the contract's 100 ms. The codes' meanings go in
   the board's documentation, since the class gives them none. A
   testbench with a codec model (an I2C slave with the register file,
   and a lock bit that sets after a delay) checks the register image
   the codec ends with, and both fault paths with their codes.
2. **The I2S output stage**, `audio_i2s_out`, board-neutral because the
   clocks come in from the codec. BCLK and LRCLK are synchronized into
   the audio clock, the next frame is requested one frame ahead, and
   each channel's 16 bits go MSB-first, padded to 32, changing after
   each BCLK falling edge. It runs on the CPU clock, about 20 samples
   a BCLK at 60 MHz, with a build check on that ratio; the rate no
   longer comes from this clock. A testbench decodes `DAC_SDATA`
   against the frames pushed.
3. **The Nexys top**: the MCLK PLL; the device behind autoconfig after
   the display, with `AUDIO_CHANNELS` as on the ULX3S and the CPU clock
   as its audio clock; the setup and the output stage; and the pins
   copied from the master XDC with their false paths.
   `tb_nexysvideo_penumbra2_top` lists the device, with a codec model
   enough for the setup to finish.
4. **Bring-up**: `paudio` attaches, `audioplay` plays a WAV and
   `pmusic-test` its listening checks through the speakers. With the
   speakers on, listen through a board power-on, a bitstream load, a
   CPU RESET, and a reload while audio plays.

On completion the board facts above move to
`doc/internals/nexysvideo/overview.md`, and this entry goes.

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

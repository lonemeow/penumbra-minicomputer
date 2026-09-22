# Hardware: devices

Peripherals behind the bus: UART, SD, display, USB host, audio.

## Hardware: UART hardware flow control (RTS/CTS)

With the 16-byte RX FIFO landed, paste loss only happens when an
inbound burst overruns the FIFO before the kernel can drain it — at
115200 baud the FIFO holds ~1.4 ms of data, and kernel ISR latency
tails (TLB miss chains during the paste itself, copyin from a long
`tty` queue update, etc.) occasionally exceed that on a 25 MHz CPU.
The proper fix is RTS/CTS hardware flow control: the FPGA tells the
host "stop sending" before the FIFO overflows.  Verified
2026-05-15 — the ULX3S board exposes the FTDI's modem-control
lines, but asymmetrically:

| Signal      | FTDI pin | FPGA pin | Direction         | Status                             |
|-------------|----------|----------|-------------------|------------------------------------|
| `ftdi_nrts` | RTS#     | M3       | FPGA reads        | wired, unconditionally available   |
| `ftdi_ndtr` | DTR#     | N1       | FPGA reads        | wired, unconditionally available   |
| `ftdi_txden`| TXDEN    | L3       | FPGA reads        | wired (RS-485 TX-enable hint)      |
| FTDI_nCTS   | CTS#     | V4       | **FPGA drives**   | **shared with JTAG_tdo**           |
| FTDI_nRI    | RI#      | R5       | FPGA reads        | shared with JTAG_tdi               |
| FTDI_nDSR   | DSR#     | T5       | FPGA reads        | shared with JTAG_tck               |
| FTDI_nDCD   | DCD#     | U5       | FPGA reads        | shared with JTAG_tms               |

(See the commented-out `LOCATE` block at the bottom of
`hw/constraints/ulx3s_v20.lpf` lines 229-232.)

The asymmetry is the critical detail: the half of flow control that
*matters for paste* — the FPGA telling the host to slow down — needs
the FPGA to drive `FTDI_nCTS` on pin V4, which doubles as
`JTAG_tdo`.  Using that pin requires:

1. Configuring the FTDI's MPSSE/D2XX side so the modem-control
   pins are routed to the UART channel rather than the JTAG channel
   (`fujprog` and ULX3S board design assume the JTAG mode at boot).
2. Either accepting that you can't reflash over JTAG while the
   FPGA is driving CTS (probably fine — `fujprog` resets the FTDI
   into JTAG mode before each flash), or wiring the pin tristate so
   the FPGA backs off during JTAG configuration.

The cheaper, JTAG-safe alternatives that *don't* fully solve the
problem but help:

- Bump `FIFO_DEPTH` from 16 to 64 (parameter is already exposed on
  `uart.sv`).  Buys 4× more buffering time, ~5.6 ms at 115200 baud
  — wide enough to absorb most kernel latency tails.  Caveat:
  com(4) detects the FIFO size from a separate IIR signature
  (TL16C750/16C950 variants) that we don't currently advertise;
  com(4) would still believe it's 16 deep and configure trigger=14
  accordingly.  Useful as breathing room but not a real fix.
- Lower the trigger threshold in com(4) (FCR[7:6]=00 → trigger=1)
  so the ISR fires on every byte instead of waiting for 14.
  Trades IRQ rate for headroom against kernel latency.

Independently of which mitigation lands, the underlying signal is
"kernel ISR latency tails > 1.4 ms occasionally."  That's worth
investigating in its own right (most likely a TLB-miss chain
during the very paste that's filling the FIFO, since the kernel's
input-processing path touches tty buffers and current LWP state).
Hardware flow control papers over the symptom but the latency tail
is its own diagnostic.

## Hardware: SD read throughput is bottlenecked by pmci_burst byte-PIO

SD reads run at ~190 KB/s through FFS ("loading a binary takes
seconds").  Root-caused by instrumentation on ULX3S @ 25 MHz CPU /
12.5 MHz SCLK (`time dd if=/netbsd of=/dev/null bs=32k` → 4.2 MB in
~22 s).  Per 512-byte block, split into card-wait vs our processing:

```
token poll (waiting on the card):   ~700 cyc   ~28 µs    ~2%
pmci_burst (our byte-PIO):        ~31,600 cyc  ~1.26 ms   ~98%
```

`pmci_burst` is ~45× the card wait and ~95% of the kernel `sys` time.
The ~1.26 ms is: fill 512 idle bytes (~287 µs CPU PIO) + clock 512
while busy-polling XFER_DONE (~328 µs) + drain 512 (~287 µs CPU PIO)
+ per-block CONTROL save/restore + FIFO flush.  All single-byte MMIO,
all sequential, bus idle during fill/drain.

**Falsified — do NOT re-investigate these:**
- the SD card (token-wait is negligible, ~28 µs/block);
- the SPI clock (wire is ~26% of the burst; the validated 6.25→12.5
  MHz bump gave only ~6%);
- transaction granularity (reads ARE coalesced to 64 KB CMD18 =
  128 blocks/cmd = MAXPHYS; FFS/buffer-cache cluster correctly);
- IRQ/cache/TLB noise (the earlier guesses here — `sys` is dominated
  by the byte-PIO, not interrupts).

**Levers, all in our control:**
1. Word-wide FIFO MMIO (4 B/access) — cuts the ~574 µs fill+drain
   PIO ~4×.  Needs HW FIFO-width + a CAP-tiered driver (byte path
   stays the boot/discrete floor; word path gated on a CAP bit).
2. Auto-idle TX for reads — let the engine clock 0xFF itself instead
   of the CPU filling 512 idle bytes; drops the ~287 µs fill.
3. Cross-block streaming — drain block N while the engine clocks
   N+1.  The 512-deep FIFO (= one block) forces today's stop-start.

Realistic: ~2-3× on the pmci term, ~1.5-2× end-to-end (the kernel
I/O stack + per-command framing is the other ~half of the run).  A
real SPI read-path + HW project, not a quick fix; gen2-adjacent.

## Hardware + kernel: local console (HDMI display + USB keyboard)

Design complete and committed as docs; implementation underway on both
halves — per-layer status in the subsections below. The goal is a
standalone local console — character-cell video out over GPDI/HDMI and a
USB keyboard in — so the machine needs no host terminal. NetBSD is the
first consumer (boot may stay on UART initially); a boot-ROM local
console is a later, well-defined follow-on.

Two new autoconfig device classes
([`system/bus.md`](system/bus.md)):

- `CLASS_DISPLAY` (6) — display adapter: a character-cell console as
  the mandatory core, plus an optional framebuffer capability
  (`CAP.FRAMEBUFFER`, geometry/format discoverable). Contract
  [`system/devices/display.md`](system/devices/display.md),
  microarchitecture [`internals/display.md`](internals/display.md).
- `CLASS_USBHC` (8) — transaction-level USB host. Contract
  [`system/devices/usb-host.md`](system/devices/usb-host.md),
  microarchitecture
  [`internals/usb-host-controller.md`](internals/usb-host-controller.md).

Decisions already settled (rationale lives in the docs, not here):

- Text cells are 16-bit `{glyph, attribute}` in 32-bit-strided slots;
  color is optional/discoverable (`CAP.COLOR`); the hardware cursor is
  mandatory; mode 0 (640×480 / 80×30) is the mandatory power-up mode for
  monitor compatibility, with extra modes optional via `MODE_SEL`.
- USB exposes a transaction-level minimum (token / data / handshake); the
  NetBSD HCD sits under the MI USB stack (`dev/ic/sl811hs.c` as the
  structural template), which owns enumeration + HID. `ukbd` → `wskbd`
  and a `pcdisplay`-style `wsdisplay` meet at `wscons`.

Implementation work, by layer:

### Display RTL, text path — first cut: mode 0 only

Bring-up order: prove the output path on glass with the test-pattern
generator before any machine-side integration. Everything up to the
GPDI connector is CPU-independent, so the physical layer's real risks
(PLL config, serializer word alignment, lane order) are isolated from
bus/autoconfig work and verified with nothing but a monitor. Machine
integration then lands against a display path that is already known
good.

1. Pixel-domain chain — done: timing generator (`video_timing`),
   test-pattern generator (`video_pattern_gen`), TMDS encoder
   (`video_tmds_encoder`), each unit-tested against an independent
   oracle in `make test-modules`.
2. 10:1 DDR serializer — done: `video_serializer`, 5x serial clock
   from the same PLL, two bits per serial cycle into an `ODDRX1F`
   owned by the board top (pure-logic module — the sdram_ctrl /
   sdram_phy_ecp5 split, one level down). The DVI clock lane is a
   fourth instance fed the constant word `10'b0000011111`, so clock
   and data share one launch path by construction.
3. Closed-loop chain testbench — done: timing → pattern → three
   encoders + clock lane → serializers, with the testbench playing the
   monitor: sample both DDR phases, character-align on the blanking
   control codes, TMDS-decode, and compare a full frame against the
   pattern oracle. With this passing, only the PLL primitive and the
   pads remained unproven.
4. Bring-up top — done: `ulx3s_video_test_top` (via the `TOP=` escape
   hatch — a probe, not a registered machine) instantiates the
   chain-test module in front of the pieces simulation could not
   prove: a dedicated video EHXPLLL at 25 MHz pixel / 125 MHz serial
   (integer PLL ratios cannot make VESA's 25.175 MHz from the 25 MHz
   crystal; the resulting ~59 Hz refresh is within monitor tolerance
   and the norm on this board) and the `ODDRX1F`-driven `gpdi_dp[3:0]`
   pads. Validated on glass: a stable test pattern, monitor syncs at
   the even-25 MHz timing. The triage ladder (no signal → PLL/clock
   lane; rolling → word alignment; wrong colors → lane order) lives in
   the top's header comment.
5. Pixel generator — done: font ROM (`video_font_rom`, generated from
   NetBSD's public-domain bold8x16 via `wsfont2hex.py`), dual-clock
   cell RAM (`video_cell_ram`), and the 4-stage scan-out pipeline
   with the blinking hardware cursor (`video_textgen`), each
   unit-tested against an independent oracle; `video_text_chain`
   closes the loop through the TMDS link, and the bring-up top shows
   the preloaded splash (`hw/video/splash.txt` → `text2cells.py`,
   UTF-8 → CP437) on glass.
6. Machine side — done: `video_display` (register block + CELLS
   aperture + 2-FF control crossings around the text chain) behind an
   `autoconfig_dev` wrapper (`CLASS_DISPLAY`, 16 KiB), wired into
   `ulx3s_penumbra1_top` with a dedicated video PLL and validated
   from the ROM monitor — autoconfig discovery, register dumps, and
   cells written over the bus rendering on the monitor. The display
   hardware ladder is complete; what remains of the local console is
   software (`wsdisplay` back-end below) and the USB keyboard half.

**Timing after the display landed.** The gen1 board top now sits on
the 25 MHz constraint rather than above it: a seed sweep spans
23.99–27.5 MHz with only half the seeds passing, so placement luck —
not logic depth — decides each build. The critical path is unchanged
by the display (the datapath → `status_reg` ESR cone, ~41.6 ns
either side of the change, with identical LUT/FF/EBR counts), and the
display's own domains have enormous margin (pixel 85 MHz against 25,
serial 339 against 125). `keep_hierarchy` on `video_display` and the
pinned seed hold it for now; the durable fix, when the margin is
wanted back, is a floorplan for this top (`LPF_DESIGN` + `PREPACK`,
as `ulx3s_penumbra2_top` already carries) or shaving the ESR cone.

Goal (later): PLL dynamic-reconfig FSM + mode 1 (800×600 / 100×37);
EDID/DDC readout when the mode list gives it a consumer (contract
space — CAP bit, `EDID_CTRL`, the `EDID` aperture — is reserved, and
the ULX3S already routes the GPDI DDC pair to FPGA pins).

### Display RTL, framebuffer capability — done

`CAP.FRAMEBUFFER` is implemented and validated on glass from the ROM
monitor: a 320×240 8bpp picture pixel-doubled onto the mode-0 raster,
its 256-entry palette, and the source select at the parallel-RGB seam.
Selecting it never re-times the output, so it needed nothing from the
mode/PLL work.

- Storage is `video_fb_ram` (per-byte write enables, since the aperture
  is mapped into a rendering process) and `video_fb_palette`; scan-out
  is `video_fbgen`. The output chain carries both sources and is named
  `video_pixel_chain` for it.
- Scan-out addressing is a counter, not an offset computation:
  framebuffer lines are contiguous, so a line's walk ends on the next
  line's first word, repeating a line reloads the start it began from,
  and the wrap at the end of the array is the frame rewind. No adder,
  no stride constant, and an address that cannot leave the array.
- Cost measured on `ulx3s_penumbra1_top`: 42 EBR (40 pixels + 2
  palette), bringing the top to 128/208. The estimate of ~34 in the
  microarchitecture notes assumed ideal bit packing; byte-wide banks
  waste the ninth bit of each block and round depth up.
- The device window grows to 256 KiB, and the display becomes the first
  autoconfig device here to consume byte enables.
- Timing after it landed: gen1 closes on 9 of 10 placement seeds, the
  pinned seed 5 at 28.31 MHz against the 25 MHz constraint.

Remaining is software: the `wsdisplay` dumb-framebuffer path in
`pdisplay` (`GINFO` / `GET_FBINFO` / `LINEBYTES`, `PUTCMAP` onto
`FB_PALETTE`, `SMODE` driving `FB_SEL`, and an `mmap` bounded to the
pixel aperture so a mapped client cannot reach the control registers).

### USB host RTL — low + full speed
- `usb_crc5` / `usb_crc16` — token and data CRC generators, the
  board-neutral MAC's first leaf cells (Galois LFSRs; CRC5 bit-serial,
  CRC16 byte-parallel for the one-byte-per-cycle datapath). The bare
  modules emit the remainder; the MAC adds the on-wire complement +
  bit-reverse. Reference-checked unit tests + a `sw/tools/usb_crc.py`
  model (`6251a87`, `b74a5ef`).
- `usb_bit_stuff_tx` — SIE transmit bit-stuffer: inserts a 0 after six
  consecutive 1s, with a consume-handshake (`o_stuff`) that back-pressures
  the serializer. Reference-checked unit test (`ca1b502`). Per-packet
  `i_init` re-seeds the run count with SYNC's terminating 1, the
  unstuffer's mirror (`768a244`).
- `usb_nrzi_encode` / `usb_nrzi_decode` — SIE line coding: a 0 toggles the
  line level and a 1 holds it (the decoder is the exact inverse). Each
  reference-checked; the decoder round-trips the encoder (`d2b915d`).
  Made line-true for chain composition (`de8bd71`): the encoder gains a
  per-packet `i_init` reloading its idle-J reference and holds `o_line`
  registered for a full bit time; both cells reset to idle J — a K-shaped
  seed makes the first post-reset sample decode as a phantom 1, which is
  SYNC's end marker.
- `usb_bit_unstuff_rx` — SIE receive bit-unstuffer: removes the stuffed 0
  after six consecutive 1s (`o_valid` drops on it) and flags a bit-stuff
  error (a 1 where the 0 was due → seam `o_rx_error`) in the same decision.
  Round-trip + error-injection unit test (`65dfccd`). Per-packet `i_init`
  re-seeds the run count with SYNC's terminating 1 (USB counts it), and
  `o_valid`/`o_error` are qualified by `i_en` for sparsely-paced
  consumers (`9f12a7a`).
- `usb_serialize_tx` — SIE transmit serializer: shifts each byte out LSB
  first across a byte handshake (`o_byte_ready`) above and the bit-time
  tick below, folding the stuffer's back-pressure in as `i_hold` so an
  inserted stuff bit holds the current bit instead of dropping it; goes
  empty on the final bit to keep the line bubble-free between bytes.
  Unit test across five pacings (gated tick + injected hold) (`dc8a57c`).
- `usb_deserialize_rx` — SIE receive deserializer, the serializer's
  mirror: shifts recovered bits in at the MSB so eight assemble into an
  LSB-first byte; `i_valid` ignores removed stuff 0s and `i_init`
  re-aligns byte boundaries per packet (a concurrent assertion guards
  that `i_init` never rides a data bit). Mirror-checked against the
  serializer's bit order + a cross-packet realignment case (`b9e9a8e`).
- `usb_oversample_rx` — SIE receive clock recovery: oversamples the
  resolved J/K line (5×/40× from `usb_pkg`) and locks a per-bit phase
  counter to NRZI edges, strobing the sampled level at each bit midpoint
  into `usb_nrzi_decode`. The edge lock re-centers to phase 1 and
  suppresses a sample that coincides with an edge, which is what holds
  lock at the 5× full-speed margin. `usb_pkg.sv` lands here for the
  PHY-internal constants (divisors, speed, J/K/SE0). Waveform-recovery
  unit test at both speeds ± edge jitter (`cfb3f07`).
- `usb_line_state` — SIE differential decode: resolves the D+/D- pair
  into SE0 / J / K / SE1, with the speed-dependent J/K swap (J is D+ high
  at full-speed, D- high at low-speed). Feeds the sampler its J/K symbol
  and the framing layer its SE0. Exhaustive unit test (`b72f044`).
- `usb_rx_framing` — SIE receive packet bracketing: SOP on the first K
  out of idle, SYNC end on its terminating 1 (an end-marker, not a
  full-pattern match, so hub-stripped SYNC still detects), payload
  gating to the unstuffer, EOP on SE0. Unit test incl. stripped SYNC +
  back-to-back packets (`6ac3503`).
- `usb_rx_test` — receive-chain integration DUT (line state →
  oversample → NRZI → framing → unstuff → deserialize), the shape of
  `usb_phy_<target>`'s receive half. End-to-end test drives raw D+/D-
  packet waveforms and requires the payload bytes back: both speeds,
  edge jitter, hub-stripped SYNC, stuffing edge cases, back-to-back
  packets (`6ac3503`).
- `usb_tx_framing` — SIE transmit packet bracketing, `usb_rx_framing`'s
  mirror: owns the bit-clock pacer (5×/40× divider — transmit generates
  timing, receive recovers it), feeds SYNC to the encoder, seeds the
  stuffer on SYNC's last bit, pumps payload until the serializer empties
  *and* the stuffer owes nothing (a packet ending in six 1s still gets
  its trailing stuff bit), then drives the SE0/SE0/J EOP raw below the
  encoder and releases. Unit test: phases, spacing, trailing stuff,
  re-arm (`1bba2b9`).
- `usb_tx_test` / `usb_loop_test` — transmit-chain integration DUT
  (serializer → stuffer → framing/encoder → line mux → registered pins,
  the shape of `usb_phy_<target>`'s transmit half) with a structural
  waveform testbench (SYNC pattern, exact stuffed payload, EOP shape),
  and the TX→RX loopback closing the loop over every SIE cell: bytes fed
  in come back out, both speeds, multi-packet (`1bba2b9`).
- **The SIE is complete**: all bit-, timing-, framing-, and line-level
  cells built, unit-tested, and proven end-to-end in both directions.
- Seam verified against the UTMI+ spec rev 1.0 (saved locally as
  `~/UTMI-PLUS-SPECIFICATION.pdf`) so a real ULPI/UTMI PHY satisfies it
  unmodified: opmode = the UTMI+ operational modes; bus reset = the
  HS-termination drive state (`xcvr_sel 00` + `term_sel 0`, SE0 by
  electrical result — FS/LS-only PHYs drive SE0 directly, caps carve-out
  documented); resume = opmode 10 + held `00h` data; LS keep-alive = the
  transceiver-decoded one-byte `A5h`; `line_state` = raw {D-, D+} with
  SE0 glitch filtering; `usb_speed_e` now carries the XcvrSelect
  encoding end to end (`6959ad2`, `0cba7d2`). `XFER_STATUS` gained
  `RXTOGGLE` — hardware ACKs CRC-good IN data and reports the received
  toggle; retransmission detection stays in software (`6959ad2`).
- **MAC — in progress.** Decomposition agreed, leaf-cell style, all on
  the 60 MHz clock (the CDC crosses later), build order as listed.
  Done: `usbhc_pkt_tx` — token/SOF/DATAx/handshake transmit, the
  on-wire CRC complement+reflect above the bare CRC cells, the bare-PID
  keep-alive transmit; PID codes in `usb_pkg`; golden-vector testbench
  anchored to `sw/tools/usb_crc.py`, including byte-per-cycle pacing
  (`0932a66`). Done: `usbhc_pkt_rx` — PID classify + check-nibble,
  packet body → buffer as received (CRC bytes included, per the DATA
  contract), CRC16-residual check, EOP-latched `{pid, len, ok, err,
  overflow}` (`87c0a57`). Done: `usbhc_txn` — token → [data] →
  handshake FSM, speed-scaled turnaround timeout + drain-aware
  inter-packet gaps, hardware host-ACK, trust-then-dispatch RESULT
  classification, IN-only rx→buffer store grant; TOKEN.PID and RESULT
  encodings in `penumbra_pkg` (`354e06f`). Done: `usbhc_frame` — 1 ms
  timer, FRAME counter, SOF/keep-alive request under the MAC arbiter's
  grant (never splits a transaction), missed markers coalesced to the
  latest frame number (`101ab5b`). Done: `usbhc_port` — debounced
  connect/speed detect from the raw line (D+ pull-up ⇒ FS, D- ⇒ LS,
  SE1 = fault), detection paused while the host drives the bus,
  reset/resume recipes, PORT_CHANGE events (`8635b75`). Done:
  `usbhc_mac` — the five-cell composition: transmit arbiter (a start
  beats a due marker, the grant follows the marker request down, and
  launches wait out a disowned marker's drain), owner-routed
  packet-transmitter mux and completion strobe, resume transmit
  override, IN-grant gate on buffer stores. The MAC-level testbench
  drives the seam at line-realistic pacing against
  `hw/sim/usb_device_sim.h` — the `UsbDeviceSim` embryo both sims will
  share (its future keyboard input must NOT take terminal stdin — the
  UART console owns it — separate pty/socket, mechanism TBD)
  (`63b0bf3`). **The MAC is complete.** Next: the swappable PHYs,
  `usb_phy_sim` / `usb_phy_ecp5`.
- `usb_phy_sim` / `usb_phy_ecp5`: compose the SIE cells behind the seam
  (`usb_rx_test`/`usb_tx_test` are the two halves' shapes); `phy_sim`
  replaces the line layer with a byte-level packet port + connect/speed
  sideband exported through `machine_sim` like the SPI/SD model, pacing
  bytes at real bit-time rates so MAC timing stays honest.
  Done: `usb_phy_ecp5` — the SIE composition plus the PHY-tier glue:
  drive-state decode for the signaling recipes (reset SE0, resume raw K
  + appended LS EOP, non-driving release), the transceiver-decoded LS
  keep-alive (lone A5h ⇒ bare EOP), receive squelch (own transmissions
  never reach the MAC; the raw line-state sideband still sees them), the
  SE0 transition-glitch filter. Pair testbench cross-wires two instances
  over a resolved bus with cycle-counted recipe waveform checks
  (`2e602f6`); the oversampler re-locks on live speed changes to survive
  the port's transceiver-code swaps (`72c6a72`). Done: `usb_phy_sim` —
  the seam with the line layer replaced by a byte-level device port:
  packet-byte pulses out (EOP/keep-alive/reset/resume as explicit
  strobes and levels), a credit handshake in (the device model owns its
  turnaround; the PHY owns bit-time pacing), connect/speed as the
  pull-up sideband. The stack testbench mates `usbhc_mac` to
  `usb_phy_sim` with `UsbDeviceSim` behind the device port — the seam
  proven tier-to-tier ahead of machine integration; the machine_sim
  export of the device port rides with the `usbhc_regs`/autoconfig step
  (`6702220`). **The PHY tier is complete.**
- Done: US2 wiring — the gen1 board top carries the machine_sim USB
  block with `usb_phy_ecp5` on the seam; D+/D- sensed and driven
  single-ended on the `usb_fpga_bd` pads (the differential-input
  pair on the same copper stays unused, so the vendor LPF is
  untouched), pulls on `usb_fpga_pu_*`, and the PLL's spare CLKOS3
  serving the 60 MHz USB clock exactly (build-failure guard on VCO
  retargets). Validated on hardware: a real full-speed device
  enumerates end to end through the NetBSD stack — descriptors,
  SET_ADDRESS, serial string — on the physical port.
- `autoconfig_dev` wrapper (`CLASS_USBHC`); machine_sim integration test
  (poll CONNECT → reset → GET_DESCRIPTOR → R1) under `make test`.
  Done: `usbhc_regs` + `usbhc_cdc` + the `usbhc` composition, proven
  from the bus at a 5:2 clock ratio by `tb_usbhc_dev` — the executable
  spec for the ISS model (`ebc65e7`). Done: the gen1 machine attachment
  and `test_usbhc_enum` — autoconfig discovery through a full
  enumeration (descriptor read, SET_ADDRESS with the old address proven
  dead, wLength-bounded re-read, STALL for an unknown request) with
  self-identifying failures (`867aa7d`). Bringing the first bare-metal
  software onto the autoconfig path exposed a pre-existing config-space
  write-stall fault the ROM's probe handler had been swallowing
  (`aa65aff`).
- Done: **ISS `CLASS_USBHC` at register level** — the register contract
  (autoconfig + TOKEN/XFER/PORT/FRAME/DATA) in `sw/sim/penumbra_iss.cpp`
  behind a two-slot autoconfig chain, transaction-level only (no
  SIE/timing model), with the shared `UsbDeviceSim` enumerating behind
  it; `test_usbhc_enum` runs under `make test-iss` (`140db78`). Driver
  bring-up (the NetBSD HCD, a ROM keyboard reader) iterates on the fast
  simulator now. The keyboard-input-not-on-stdin constraint applies when
  the HID personality lands.

### Kernel
- `ukbd` + `wskbd` binding for HID keyboards.  The host-controller
  driver, the MI USB stack, and `uhidev`/`uhid` are done — keystrokes
  reach userland through `/dev/uhid*` — but a `wscons` console needs
  the keyboard bound as a `wskbd` input instead.
- Done: `wsdisplay` back-end for `CLASS_DISPLAY` (`pdisplay`, emulops
  over the `CELLS` aperture with a RAM shadow for the copy ops) plus
  the keyboard half — `ukbd` → `wskbd` → the display's kbdmux. Proven
  on hardware: keystrokes reach `ttyE0` and echo on the monitor, so
  the local console is a working terminal. `wsconsctl` state, the
  `MODE_MAPPED` mmap path, and the `EXTGLYPHS` range beyond ASCII
  remain unexercised.
- Local console, remaining: a session on `ttyE0` at boot needs
  multi-user (`init` reading `/etc/ttys`, whose entry is in place) —
  today a getty must be started by hand from the single-user shell.
  Making the display the *kernel* console (boot messages on glass, DDB
  on the local keyboard) additionally needs `wsdisplay_cnattach` plus
  a polled console keyboard: `ukbd_cnattach` requires interrupt-pipe
  polling in `pusbhc`, without which the console would have output but
  no input between attach and USB enumeration.

### Boot ROM — later
- Per-class console backends (text-cell writes; framebuffer software
  glyphs once that class lands) and a USB boot-keyboard reader, behind a
  putc/getc abstraction the monitor selects at autoconfig.

## Hardware: USBHC v2 — multi-transaction transfers, NAK pacing, PRE

Three planned upgrades to the transaction engine.  The first two target
the two
dominant software costs of the one-transaction-at-a-time design:
every 64-byte transaction costs one interrupt plus a PIO drain or
refill of the DATA window, and a NAKing endpoint costs an interrupt
per poll.  Full-speed bulk fits ~19 packets per frame (~1.2 MB/s);
per-transaction interrupt handling caps a 25 MHz CPU well below
that, and the interrupt tax is felt system-wide (binary IPL, single
wire-OR'd line).  Both features remove interrupts rather than add
wire bandwidth.

- **Multi-transaction transfers.**  Grow the DATA buffer (one 18 Kb
  BRAM covers 2 KB; the CAP register already advertises the size and
  the driver honors it) and let the engine chain max-packet
  transactions within one transfer: keep issuing tokens while ACKs
  continue and buffer remains; stop on buffer-full, short packet,
  NAK, or error.  One completion interrupt per transfer chunk — a
  512-byte disk block or a ure RX aggregate completes as one event
  instead of 8–32.  Toggle tracking for the chained run moves into
  hardware (software seeds it per transfer); XFER_STATUS grows a
  byte count, short-packet flag, and terminating result; the frame
  timer must gate chain segments that cannot finish before EOF.

- **Hardware-assisted NAK retry.**  On NAK, re-arm the latched
  transaction at the next frame tick (optionally a per-transaction
  interval field for interrupt endpoints) instead of raising
  XFER_DONE; interrupt only on a non-NAK outcome or a software
  cancel.  An idle interrupt endpoint or a not-ready bulk device
  then costs zero interrupts.  Open design points: the cancel
  handshake (cancel-versus-completion race), and port sharing — a
  parked retrying transaction occupies the single engine, so either
  software preempts it around other traffic or the engine grows a
  second periodic-only slot (the ISP1362 INTL/ATL shape, scaled
  down).

- **PRE packets — low-speed devices behind a hub.**  The one known
  functional gap rather than a performance one.  A full-speed hub
  forwards to a low-speed port only when the host prefixes the
  transaction with a PRE packet sent at full speed and then signals
  the transaction itself at low speed, so the transmitter must
  switch bit rates mid-transaction and the receiver must expect a
  low-speed reply on a full-speed segment.  Direct-attached
  low-speed devices already work; today an older keyboard that
  works on the port goes silent behind a hub.  Wanted for the
  exhibit topology, where everything hangs off one powered hub.

Both must ride the device contract compatibly: new capability bits
and control fields that old kernels never set, with the existing
single-transaction register behavior as the default.

## Hardware: audio device (`CLASS_AUDIO`)

The ULX3S has a 3.5 mm jack driven by a 4-bit resistor ladder per
channel, and nothing in the design uses it. Register interface and
driver model are specified in
[devices/audio.md](system/devices/audio.md); `CLASS_AUDIO` is registered
as class 7. This entry records the measurements behind the shape of that
spec and the decisions that still need hardware to settle.

### Why the device has no addressable sample memory

The bus expresses exactly one master. Every control signal in
[bus-protocol.md](hardware/bus-protocol.md) is marked master-to-slave and
there is no request/grant pair, so a device cannot fetch its own sample
data; anything it plays, the CPU must push. That leaves two shapes: a
CPU-filled BRAM the channels address (Paula/GUS), or per-channel FIFOs.

Sample-header survey of 50 four-channel modules across ten composers
(4-Mat, Captain, Dizzy, Heatbeat, Jester, Jogeir Liljedahl, Lizardking,
Mantronix, Moby, Romeo Knight), total sample bytes per module:

```
median=90K  mean=113K  max=382K
  fits in  32K: 14/50 (28%)     Jester/elysium        102K
  fits in  64K: 19/50 (38%)     4-Mat/crocketts theme  88K
  fits in 128K: 34/50 (68%)     Heatbeat/bicykl0pedia 382K
  fits in 384K: 50/50 (100%)
```

Addressable sample memory is therefore not worth its area: 64 KB (32
EBR) plays 38% of real modules, and 128 KB reaches only 68% while taking
EBR occupancy to ~95%. Per-channel FIFOs play all of them, at ~16 EBR
and roughly 1% of the CPU — four channels at the maximum Amiga rate
(period 108, ~33 kHz) is 132 KB/s, pushed as words holding four 8-bit
samples each.

The cost is that FIFO contents belong to one source stream at one rate,
so a change of source material needs `FLUSH` rather than a queued
handoff. A change of *pitch* needs nothing: stream position is the
integral of `INC` over elapsed time, so a queued channel and an
unqueued one behave identically under vibrato or portamento.

### The DAC ladder compensates for the FPGA pin's own resistance

Ladder values from the board schematic (R14-R17 per channel): 130 Ω
(MSB), 270 Ω, 549 Ω, 1.1 kΩ. The same sheet states the pin's own
output resistance as 10 Ω and derives the board's 75 Ω figure from it.
That 10 Ω is part of the design: including it the legs are
140/280/559/1110 against an ideal binary 140/280/560/1120, for a
worst-case DNL of ±0.013 LSB. The odd-looking 549 and 1.1 k exist so
the sum lands on the binary sequence; computing weights from the bare
resistor values instead yields +0.34 LSB and a ladder that only looks
mismatched.

The compensation is exact at one pin resistance — worst-case DNL runs
+0.336 at 0 Ω, ±0.013 at 10 Ω, −0.332 at 20 Ω — and the ECP5's drive
impedance moves with process, voltage and temperature. Worse, it
differs between sourcing and sinking, which makes each leg's weight
depend on that leg's own state. No resistor value trims that out.

Noise shaping does not fix ladder nonlinearity: the static error acts
on the code stream unshaped, intermodulating out-of-band shaped noise
back into the audio band. Quantization itself is a non-issue —
second-order shaping at OSR 512 puts it ~148 dB down.

### Simulated: leg choice matters more than code width

`make audio-sweep` drives the modulator against a model of the ladder
with the pin's source and sink resistance swept. The code stream does
not depend on what the ladder does with it, so one capture feeds every
parameter point. Stimulus is a two-tone pair into 10 kΩ; the
difference product exists only through nonlinearity.

Two results survived contact with hardware: a one-bit code is exactly
invariant under any asymmetry (two levels define a straight line), and
which legs a code drives matters more than how many, because the pin
resistance perturbs each leg in proportion to its conductance — ±2 Ω
is 1.4% of the 140 Ω leg and 0.18% of the 1110 Ω one.

The magnitudes did not: the model was 12 dB optimistic on the four-bit
case, and predicted the wrong winner. It has no term for switching
current, which hardware showed dominates both the distortion and the
noise. Treat it as a ranking tool, not a predictor.

### Shortening the modulator loop to close 60 MHz

The loop was two serial 20-bit adds plus two saturations and the
quantizer: 18.95 ns against a 16.67 ns period, 52.8 MHz achieved. Four
changes took it to 12.88 ns and 77.6 MHz, none of them altering the
transfer function or moving the measured figures by 0.4 dB:

- the DAC reconstruction is a sixteen-entry table rather than a shift
  and a subtract, the code being at most four bits;
- the quantizer's offset lives in v2's reset value, since a constant
  added to both sides of an integrator's recurrence cancels;
- the input scale is registered, being feedforward rather than part of
  the loop;
- v2 subtracts the feedback from its own registered state in parallel
  with v1's sum, rather than behind it.

Latency is what made three of those free: at these rates a sample can
be delayed by a hundred clocks and remain invisible, so anything not
inside the feedback path can be registered at will. Only the loop
itself — v1 and v2 through the quantizer and back — has to stay
combinational, and that is what the four changes shortened.

This matters because the sample rate comes off the 60 MHz USB clock
precisely so it does not follow a retuned system clock.

### Ladder pins are registered at the pads, and it changes nothing

The four legs must switch together: skew between them would present
the ladder a code that was never commanded, and that glitch energy is
code-correlated, which a measurement cannot tell from nonlinearity.
Registering at the pads packs the registers into the IO cells and took
the worst output path from 6.37 ns over eight hops to 2.5 ns over two.

It moves no measured figure — the four-bit configuration reads −40.6 dB
third-order either way, to the decimal place, with the same noise
floor. Skew is therefore not a contributor, which is the third
mechanism excluded by measurement rather than argument. The registers
stay for the output timing they give, not as a fix.

### Measured on hardware: two bits on 549/1.1k

Seven configurations recorded through the jack into a line input,
two-tone stimulus (9000 + 10125 Hz), 25 s captures at 0.084 Hz
resolution, sorted by the figure that decided it. "Noise" is absolute
in-band; "matched SNR" is what a listener hears once the
configurations are level-matched.

| config | peak leg mA | 3rd-order | noise | matched SNR |
|--------|-------------|-----------|-------|-------------|
| 2b {549,1.1k}     |  5.9 | −87.4 | −128.8 | **108.8** |
| 1b {1.1k}         |  3.0 | −83.4 | −134.5 | 105.2 |
| 2b {270,549}      | 11.8 | −89.1 | −123.3 | 103.3 |
| 3b {270,549,1.1k} | 11.8 | −53.7 | −123.1 | 103.1 |
| 1b {130}          | 23.6 | −79.4 | −120.8 | 100.8 |
| 4b {all}          | 23.6 | −40.6 | −120.6 | 100.6 |
| 3b {130,270,549}  | 23.6 | −50.4 | −119.8 |  99.8 |

Distortion is measured on the **third-order** pair (2f1−f2, 2f2−f1).
Ladder mismatch is odd-order; the recording chain's own distortion is
even-order and lands on f2−f1 and f1+f2, so the converter can be
measured through a chain far dirtier than itself. Reading the
difference tone as the converter's condemns every configuration alike
at about −45 dB. The one-bit rows are the floor — their static
transfer function cannot bend — and both two-bit configurations
measure below them.

**Two bits is the ceiling.** The third bit costs 35 dB, a cliff rather
than a gradient, and far more than any weight-ratio error produces.
Two candidate mechanisms are excluded by measurement: the same
configuration reads identically at 30 MHz and 60 MHz, which rules out
settling and edge asymmetry; and a two-bit converter on the 270 Ω leg
is among the cleanest, which rules out one bad leg.

**Noise decides it, not distortion.** Absolute in-band noise rises
about 5.7 dB per doubling of the largest driven leg's current,
monotonically across all seven. It is not quantization noise — three
configurations share a 0.22 V step and span 14 dB of noise, and at
this oversampling even one bit has ~135 dB of in-band SQNR. Through
amplified speakers the two configurations that avoid the 130 Ω and
270 Ω legs are audibly clean and the rest hiss; no configuration
sounds *bad*, so the choice is between margins.

Two measurement notes that inverted conclusions. Noise relative to the
tones ranks the configurations backwards, because each was captured at
its own span; only absolute figures are monotonic. And level-matching
is what exposes the noise at all, since digital attenuation scales the
signal and not the analog floor — so the probe scales the sample clip
per configuration and deliberately leaves the measurement tones alone.

Design consequence: **two bits on 549/1.1k**, driving neither
high-current leg. 0.66 V p-p (234 mV RMS) is line level unaided. Two
bits on 270/549 measures 1.7 dB better on distortion and is the wrong
choice: its 270 Ω leg costs 5.5 dB of noise.

### The jack is DC coupled, and the intended load is line level

There is no series capacitor between ladder and jack — the schematic
shows only R14-R17 in the path, and the board manual describes the
output as "3-channel 4-bit DAC with 75 ohm output impedance and 0-3.3V
DC swing".

The intended load is not headphones. Exhibit use drives amplified
speakers or the line input of a capture card, both high-impedance and
AC-coupled at the receiver. Three consequences:

- **DC is a non-issue.** Into 10 kΩ the midscale rest level draws
  0.16 mA and the receiver blocks it. Into 16 Ω earphones the same
  rest level would put ~18 mA and ~5 mW of DC through the transducer,
  which is why `CTRL.ENABLE = 0` driving the pins low rather than
  parking at midscale is worth keeping as an option.
- **Full scale is hot.** 3.3 V p-p is 1.17 V RMS, about 13 dB above
  the −10 dBV consumer nominal, so a sensible operating level sits
  well below digital full scale. The headroom is free: the modulator
  measures 96 dB SNR at four bits.
- **Out-of-band energy matters more**, not less. A transducer filters
  it; a line input does not, and the board has no reconstruction
  filter. This is the second argument against narrow codes.

### Clock source must not follow CPU_HZ

Sample rate cannot derive from the CPU/system clock, which is retuned
per timing closure. The USB tier already solves this shape —
`ecp5_pll_aux_div` returns 0 unless the VCO is an exact multiple, so an
inexact retarget fails elaboration instead of detuning the peripheral.

Both instantiated PLLs are constrained, and their VCO factorisations
decide what is reachable:

| clock             | factorisation  | exact 48 kHz? |
|-------------------|----------------|---------------|
| system VCO / USB 60 MHz | 2⁸ × 3 × 5⁷ | yes (÷1250) |
| video VCO 625 MHz | 2⁶ × 5¹⁰       | no            |
| *48000*           | *2⁷ × 3 × 5³*  | —             |

The video PLL has two free outputs (`ENCLKOS2`/`ENCLKOS3` tied off) and
is crystal-fed, so it looks like the obvious home — but 625 MHz has no
factor of 3 and only 2⁶, so it cannot produce 16/32/44.1/48 kHz by any
downstream divider. It reaches 8 kHz exactly, and otherwise only
oddities like 25/40/50 kHz.

Preferred: share the existing 60 MHz USB clock, which adds no new VCO
constraint because USB already imposes it, and divides exactly across
the whole 48 kHz family (8/16/24/32/48/96 kHz). Portable fallback for a
top without a USB tier: a third EHXPLLL (2 of 4 used), crystal-fed at a
600 MHz VCO — 2⁹ × 3 × 5⁸, reachable from 25 MHz as CLKI 1 / CLKFB 24,
and exact at ÷12500. 44.1 kHz factors as 2² × 3² × 5² × 7² and is
unreachable from any of these; design around the 48 kHz family.

### Sizing, and what needs re-measuring

Last full synthesis showed `DP16KD 134/208` — 74 EBR (~148 KB) free,
`MULT18X18D 0/156`, `EHXPLLL 2/4`. Eight channels of 4 KB is 16 EBR,
taking occupancy to ~72%. Audio RAM is read at the output rate, touches
no critical path, and can place loosely, unlike the floorplanned SDRAM
and D-side cones.

- The EBR figure predates in-flight L2 geometry changes and should be
  re-measured before it is planned against.
- A 4 KB FIFO holds 124 ms for a channel draining at 33 KB/s but only
  21 ms for a 48 kHz stereo stream at 192 KB/s. Either give channel 0 a
  deeper FIFO — `DEPTH` is per channel precisely so this is legal and
  discoverable — or set `__AUDIO_BLK_MS` low enough that two blocks fit.
- The mixer's multiply-accumulate can be time-multiplexed across all
  channels: 1250 clocks per output frame at 60 MHz against roughly four
  clocks per channel. Channel count is bounded by register-file size,
  not by mixing throughput, so `INTERP` (one extra MAC per channel) is
  affordable rather than a luxury.

### Bring-up order

The two halves are independently testable, so neither blocks the other
for long:

1. Output stage alone — a synthesized signal into the modulator and
   ladder, `CTRL.ENABLE` only, needing no FIFO, no channels and no
   driver. What it confirms is the [asymmetry
   tolerance](#the-dac-ladder-compensates-for-the-fpga-pins-own-resistance):
   a two-tone stimulus, because a difference tone appears only through
   nonlinearity and is unmistakable where harmonic distortion on a
   single tone is not, and silence, for the noise floor.
2. One channel with a FIFO, plus the `audio(4)` binding. That is the
   whole `CLASS_AUDIO` minimum protocol and gives a working
   `/dev/audio`.
3. Remaining channels, `INC`, `VOL` and the mixer — the point at which
   `CAP.CHANNELS` becomes greater than one and the
   [voice interface](system/netbsd/design-notes.md#multi-voice-binding)
   becomes reachable.
4. A tracker replayer in userland, with its mixer removed and its
   per-tick channel state submitted to `/dev/pmusic` in its place.

Step 1 is worth doing before any of the register interface exists, but
most of it is a simulation rather than a listening test: the modulator's
code stream does not depend on the ladder, so one captured stream feeds
a swept model of the analog mapping. Hardware is needed only for what
the model cannot supply — the ECP5's actual source/sink impedance.

### Mono channels (`CAP.MONO`)

The device implements `PITCH` and `VOLUME` but not `MONO`: `CAP_VALUE`
leaves bit 11 clear, the channel `CTRL` decode handles only `FLUSH`,
and each FIFO entry is one stereo frame. Every voice a tracker plays is
mono and placed by `VOL`, so `/dev/pmusic` writes each sample into both
halves of a word.

`MONO` packs two frames per word, which halves the stores to a channel's
data port — the refill's per-frame cost — and doubles the frames a
channel holds, so its watermark comes due half as often. It needs:

- the channel FIFO to unpack two frames per word, with `LEVEL` and
  `DEPTH` counted in frames of the selected format, as the spec
  already requires;
- the ISS model to match;
- `pmusic_push()` to pack frame pairs when `CAP.MONO` is set — pairing
  across a pass boundary, and padding a sound that ends on an odd
  frame.

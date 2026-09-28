# Hardware: ULX3S board

Work bound to the ULX3S board. Its fmax and placement findings stay
in `hw-core.md`, beside the core and memory work they gate.

## Build system: guard flashing against marginal timing

`make flash` packs and flashes whatever nextpnr produced, including a
build that missed its frequency constraint (`--timing-allow-fail`) or
one that passed with near-zero margin — and a pinned placement seed
goes stale the moment the netlist changes, so a routine RTL edit can
silently produce a 25.07-MHz-on-25 bitstream whose register-tier
flakiness then masquerades as logic bugs.  Add a guard: after PnR,
compare the achieved CPU-clock fmax against the constraint plus the
project's safety floor; refuse to pack/flash below it unless
explicitly overridden, and print the figure either way.  Pair it with
the working rule that any RTL change re-runs the seed sweep before
the pin is trusted.

## Board workflow: flashing, reset, and persistence annoyances

Four friction points in the daily flash-and-test loop, roughly in
order of pain:

- **Persistent default bitstream.**  Every power cycle currently
  loses the FPGA configuration and needs a fresh `make flash` (an
  SRAM load).  The ULX3S SPI configuration flash can hold a
  bitstream the ECP5 loads at power-on; teach the flow to program it
  (openFPGALoader writes it with `-f`) — likely as a separate
  `make flash-persistent` target so the fast SRAM load stays the
  development default.

- **SD card wedged by reset.**  A board reset or reflash that lands
  mid-SPI-transaction parks the SD card in a state the ROM's init
  cannot recover — only a power cycle clears it.  Debug the recovery
  path: the SD-SPI init sequence should be able to rescue a card
  abandoned mid-transaction (dummy clocks with CS deasserted, CMD0
  retry discipline, and if need be a longer pre-init clocking burst)
  instead of forcing the Vcc cycle.

- **Multiple bitstreams in flash, selectable at boot.**  The ECP5
  supports multiboot configurations in SPI flash (golden/update
  images with jump records selecting which loads).  Investigate what
  the open toolchain (ecppack/ecpmulti) supports and whether a
  button-selected boot image is achievable on the ULX3S — would let
  a known-good image and an experimental image coexist.

- **Reset-button press needed after flash.**  After an SRAM-load
  flash the design comes up needing a manual `btn[1]` press before
  it runs correctly.  Investigate why the post-configuration state
  differs from the post-reset state — likely something in the top's
  reset generation (PLL lock vs. release timing, or state the reset
  clears that configuration does not) — and make configuration
  come up clean.

## Placement: re-pin the default seeds

The default nextpnr seeds (`DEFAULT_SEED_ulx3s_*` in the Makefile)
were swept on main's RTL as of 5356037. Three later commits change
what yosys builds on the ULX3S tops: 2920dbc gives each L2 tag way a
memory of its own (4 block RAMs where the packed array took 8, on
every top), fe677f1 gives the gen2 L1 data arrays one explicit write
port (with 2920dbc, the gen2 machine is 75 LUT4s, 150 PFUMXes and 4
block RAMs smaller), and 265db02 moves the UART FIFOs into block RAM.
A seed chosen for one netlist says nothing about the next. Open:
sweep the three default tops, pin the best seed of each, and run each
on the board.

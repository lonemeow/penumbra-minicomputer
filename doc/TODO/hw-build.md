# Hardware: build system and board workflow

Build/test matrix, HDL frontend, flashing and board handling.

## Hardware: build/test restructure to the BOARD×CORE matrix

`doc/internals/build-system.md` defines the target structure: the
four-axis build matrix, the `hw/rtl/machine/` integration layer, the
`isa/` conformance split for test programs, and the
`make fpga BOARD=<board> CORE=<generation>` porcelain. The
test-program side of the migration is done: programs live in `isa/` /
`penumbra1/` / `penumbra2/` with `; RUNNER:` / `; REQUIRES:` tags,
`hw/tools/run-prog-tests.py` drives `make test CORE=<core>` /
`make test-prog`, and `tb_penumbra2_prog` is the generic gen2 runner.
The FPGA side is done too: sources compose from `SRC_CORE_<gen>` /
`SRC_FABRIC` / `SRC_BOARD_<board>`, board tops live under
`hw/rtl/fpga/<board>/` (`ulx3s_top` became `ulx3s_penumbra1_top`),
the Makefile registry (`FPGA_TOPS`, per-top `FPGA_SRC_<top>`,
`FPGA_ROM_TOPS`/`FPGA_UCODE_TOPS` for hex embedding) hard-errors on
unknown BOARD/CORE/VARIANT combinations, and `TOP=` remains the
low-level escape hatch. The gen2 bare-core timing probe
(`ulx3s_penumbra2_probe_top`, `make fpga/timing BOARD=ulx3s
CORE=penumbra2 VARIANT=probe`) is in: core + `unified_mem`, IRQs on
buttons, commit/retire XOR-folded onto the LEDs so synthesis keeps
the design — the synthesize-after-every-change workflow for gen2,
in place *before* the BRAM L1 lands (Decision 11's 4-way-vs-leaner
choice is gated on the IF2 tag-compare/way-mux path at synthesis).
First bare-core build: ~55 MHz achieved on ECP5-85F sg6, critical
path in ID decode toward the fault-vector register.

The gen2 machine assembly is done: `machine_penumbra2` under
`hw/rtl/machine/` honors the program-end contract, the runners build
`machine_penumbra2_sim` (machine + `boot_rom` + the full SDRAM stack), the probe
variant wraps the machine — the IF2 tag-compare/way-mux path and the
L1↔L2 layer are in front of nextpnr; the first `make timing` run of
that configuration is pending — and the ISA-shaped gen2 programs
(smoke, branch, loadstore, fault, eret, syscall_trap) moved into
`isa/`. `test_intr` stays generation-pinned: its RUNNER tag selects
the IRQ-driving testbench, which is implementation-pinned by
definition. Remaining:

1. Opportunistic: extract `machine_penumbra1` from `machine_sim` /
   `ulx3s_penumbra1_top` so both wrappers share one integration
   (the sim-vs-FPGA congruence argument in the build-system doc).

The microarchitecture sub-variant axis is in: `CORE=penumbra<n>_<sub>`
(first instance `penumbra2_5`) builds its base generation's RTL with the
`CPU_VARIANT` core parameter set (entered as the `PENUMBRA_CPU_VARIANT`
define), across `make test` / `simulate-rtl` / `benchmark-rtl` / `fpga`.
`CORE_BASE`/`CORE_SUB` split the name; the variant inherits the base's
runner, capabilities, and program suite plus its own
`hw/sim/programs/penumbra2_5/`, and for fpga `TOP` names the per-variant
artifact while `TOP_MODULE` names the shared synth module (no duplicate
board top). The gen2.5 microarchitecture is built by **composition**:
`CORE=penumbra2_5` selects the gen2.5 fileset — gen2's leaf cells (ALU,
regfile, scoreboard, decoder, the MMU/TLB/cache stack, ...) shared
unchanged, plus the forked integration chain (machine shared; core /
spine / ID + EX assemblies forked into `hw/rtl/penumbra2_5/`) — so gen2
stays byte-frozen and never reads as a gen2.5 with its optimizations
switched off. The sim build resolves the forks via a variant `-I`
prepend, the fpga build via `SRC_CORE_penumbra2_5`; the cpuid name
("Penumbra/2.5") still rides the `PENUMBRA_CPU_VARIANT` define, and
cycle-exact tests gate on a `pinned-stalls` capability gen2 provides and
gen2.5 drops. Mechanism: `doc/internals/penumbra2/overview.md` (gen2.5
organization) and `doc/internals/build-system.md` (variant naming).
gen2.5 is no longer identity-only — see its feature status in
hw-penumbra2.md.

## Hardware: evaluate yosys-slang as the SystemVerilog frontend

Synthesis reads SystemVerilog through `sv2v`, which rewrites the whole
fileset to Verilog-2005 before Yosys sees it. The seam works, but every
line the design writes is translated first, and two of the workarounds
the flow carries exist only because of that translation — they describe
nothing about the hardware.

`hw/tools/inline_hex.py` is one. sv2v emits a zero-fill loop ahead of
each `$readmemh`, and Yosys lets the loop win, so every preloaded memory
— boot ROM, microcode, font, splash — would synthesize to zeros. The
script strips the loops back out of the generated Verilog.

The character generator's palette is the other. An unpacked array
indexed by a plain signal is the idiomatic way to write a lookup table;
sv2v flattens it to a packed vector whose part-select offset is the
index times the entry width, arithmetic the source never had. Yosys maps
that multiply onto a DSP block, so a 16-entry color table costs a
MULT18X18D. The workaround selects the entry by comparison instead.

`yosys-slang` reads SystemVerilog directly into RTLIL, removing the
translation step and, with it, the cause of both. Worth measuring rather
than assuming: the acceptance test is whether `inline_hex.py` can be
deleted and the palette can go back to a plain array index, with cell
counts on a full board top no worse than the current flow's.

One calibration so the evaluation stays honest — not every DSP surprise
is sv2v's. The cell address multiplies the character row by a constant
column count, and Yosys folds only power-of-two constant multiplies, so
80 reaches DSP inference whatever the frontend. Spelling that product as
the shifts its constant implies stays necessary either way, and a slang
trial that appears to remove it means something else changed.

The toolchain image is the prerequisite: the pinned OSS CAD Suite build
has no slang plugin (`read_slang` is not a command), so this needs the
container the wrapper scripts drive rebuilt or updated. The build-time
defines the flow passes at the sv2v step (`SDRAM_PHASE_DEG`,
`PENUMBRA_CPU_VARIANT`) need an equivalent on the new frontend.

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

## Build system: Vivado thread count

Vivado caps itself at 8 threads (`general.maxThreads`), while its
container sees every host core (16 on the development machine). On
the Nexys Video gen2.5 machine top, synthesis is effectively serial
(3:08 of CPU over 3:10 elapsed), so more threads could only speed up
place, route and the analysis steps. Open: once builds get large (the
DDR3 machine), time one build at 8 threads against one at 16; if place
and route shrink noticeably, pass a make variable through to
`hw/tools/vivado/build.tcl`, kept adjustable so a concurrent LLVM
build still gets its share of the cores.

## Board workflow: flashing, reset, and persistence annoyances

Four friction points in the daily flash-and-test loop, roughly in
order of pain:

- **Persistent default bitstream.**  Every power cycle currently
  loses the FPGA configuration and needs a fresh `make flash` (an
  SRAM load).  The ULX3S SPI configuration flash can hold a
  bitstream the ECP5 loads at power-on; teach the flow to program it
  (fujprog writes flash with the right flag) — likely as a separate
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

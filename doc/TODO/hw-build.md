# Hardware: build system

Build system and test matrix, shared by every board. Work bound to
one board is in that board's file (`hw-<board>.md`).

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

## Hardware: each board top builds its own device complex

Every machine top wires the devices around the machine by hand: the
address decode, the autoconfig chain, the response fabric and the
interrupt lines. The ULX3S and Nexys Video tops share none of it, so
a new device is wired twice, and the two can drift apart. Open: once
both boards carry the same devices (`hw-nexysvideo.md`), extract the
shared complex into one module both tops instantiate, as a change
with no function of its own, proven on both boards. Extracting it
sooner guesses the seam from tops that still differ.

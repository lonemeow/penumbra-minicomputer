# Hardware: build system

Build system and test matrix, shared by every board. Work bound to
one board is in that board's file (`hw-<board>.md`).

## Hardware: gen1 has no machine module

Every pipelined generation has a `machine_<generation>.sv` under
`hw/rtl/machine/` that its sim wrapper and its board tops both
instantiate, so what simulates is what synthesizes
(`doc/internals/build-system.md`, "The machine layer"). gen1 has none:
`machine_sim` and `ulx3s_penumbra1_top` each assemble the gen1 machine
themselves, and they agree only by inspection. Open, opportunistic:
extract `machine_penumbra1` from the two so both wrappers share one
integration.

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

The pinned OSS CAD Suite build carries the plugin: `yosys -m slang`
loads it, and `read_slang` is a command only once it is loaded. Nothing
larger than one module has been read through it. The build-time
defines the flow passes at the sv2v step (`SDRAM_PHASE_DEG`,
`PENUMBRA_CPU_VARIANT`) need an equivalent on the new frontend.

## Hardware: a module's stated clock rate is never compared with the clock built

A module whose logic depends on its clock's rate (a baud divider, an
oversampling count, a management clock) is told the rate in a
parameter. Nothing ties that parameter to the clock on the module's
port, so a clock retuned for one consumer can detune another in a
build that succeeds.

What holds today:

- The CPU clock is right by construction. A top states `CPU_HZ` once,
  computes the PLL or MMCM dividers from it (`ecp5_pll_pkg`,
  `xc7_mmcm_pkg`, exact or the build stops) and passes the same
  constant to every consumer.
- A clock shared for its rate has one consumer that checks. The
  ULX3S's 60 MHz clock serves the USB host and the audio output, and
  `usbhc` stops a build whose `USB_CLK_HZ` is not the rate its
  oversampling is built for. The ECP5 PHY counts by the same factors
  with no check of its own, and is covered only because both tops put
  it on the controller's clock.
- A rate parameter with no default has to be given: Verilator and
  sv2v both stop on an instance that leaves it out. `usbhc`, `eth` and
  `eth_mdio` are written so, with their checks letting through the
  zero sv2v writes as a stand-in (`build_check`'s header has why).

Open:

- **Rate parameters that default.** `uart`, `spi`, `i2c_master`,
  `audio_i2s_out` and `audio_rate_divider` each default their clock's
  rate, so a top that leaves it out builds at a plausible wrong one.
  Dropping the defaults means giving the rate wherever a testbench
  takes one of them as its top.
- **The clock as built.** The right constant does not help a top that
  wires another clock to the port, or a clock module with fixed
  dividers (`nexysvideo_ddr3_clk`) edited without its consumers.
  Vivado knows every clock's period once it has read the MMCM and PLL
  attributes and the XDC, so a signoff gate
  (`hw/tools/vivado/gates.tcl`) can compare: a marker on the clock
  carrying the rate the module was told, and a gate that fails where
  the period of the clock reaching it differs. nextpnr derives no PLL
  output rates, so the ECP5 flow would go on relying on construction.

## Hardware: the lint gate does not check SystemVerilog's type rules

Every simulation build and `make fpga-lint` check the RTL with
Verilator `-Wall`, and the coding standards name no other checker.
Verilator warns about what simulates or synthesizes wrongly: widths,
signals unused or undriven, latches. Where the language's types
disagree and the bits still come out right it says nothing, so code
that passes the build can carry a list of warnings in an editor that
checks with slang.

A sample: fifteen modules that pass Verilator (the Ethernet cells,
`i2c_master`, the Nexys codec setup, `uart`, `spi`) through
`slang-server` 0.3.0 with no configuration gave 69 warnings.

| Warning | Count | Where |
|---------|-------|-------|
| `sign-compare`, `arith-op-mismatch` | 48 | 43 of them on a size cast of a plain integer |
| `case-enum-explicit` | 5 | a `case` on an enum that leaves values to its `default` |
| `case-redundant-default` | 4 | a `unique case` with a `default` |
| `sign-conversion` | 4 | `longint` arithmetic in a parameter |
| `empty-output-connection` | 3 | an output left open, `.o_x ()` |
| others | 5 | an import or a signal not used, a `case` with no `default` |

The commonest comes from a spelling the tree uses throughout, a
constant sized where it is used: `COUNT_W'(1)`. A cast with a width in
front resizes its operand and keeps its signedness, so the constant is
signed beside an unsigned register. The operation is still done
unsigned, as the language does whenever one operand is; only the types
disagree. A cast to a type (`typedef logic [W-1:0] count_t`, then
`count_t'(1)`) or a constant declared with its type satisfies both
checkers.

Open:

- **A target.** The FPGA container's yosys loads its slang plugin
  (`yosys -m slang`), and `read_slang -Wextra -Wconversion` on one
  module printed the type warnings the editor shows for it. A target
  would read each registered top's fileset that way, as `fpga-lint`
  does through Verilator. No whole top has been read through it, so
  what the primitive stubs and the build-time defines need is unknown.
  The same reader is the synthesis front end under evaluation (this
  file, "Hardware: evaluate yosys-slang as the SystemVerilog
  frontend"); a build that reads the design through it gets these
  warnings with it.
- **Which warnings stop a build.** Some of slang's argue with how the
  tree is written on purpose: an output left open (Verilator's
  equivalent is switched off for `fpga-lint`), a state `case` ended by
  a `default`. The set has to be chosen, and named in the coding
  standards beside Verilator's.
- **The backlog.** Size casts of integers are in about forty files
  under `hw/rtl`. Until they are cleared the target can report but not
  gate.

## Ruled out

- **One device module shared by the board tops.** Extracting the
  devices the ULX3S and Nexys Video tops both carry into one module
  both instantiate, so a device is wired once. Each top composes the
  devices its board has, and boards differ in what they have (HDMI,
  an Ethernet PHY, USB on a mezzanine card); a shared module would
  lose a device for every board without it, and would split apart as
  boards were added. The logic that must not drift between tops is
  already shared as modules (`autoconfig_dev`, `bus_devsel`,
  `bus_response_fabric`, the devices themselves); what each top
  repeats is the instance wiring, which is where boards differ.

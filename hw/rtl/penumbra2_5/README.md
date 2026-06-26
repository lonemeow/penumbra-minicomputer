# Penumbra/2.5 — composition over gen2

> **⚠️ ABANDONED — superseded by gen3.** gen2/2.5 topped out at 37.5 MHz with
> no headroom for the remaining features (store buffer, wider L1↔L2, return
> prediction); the in-order pure-stall memory-hit cone is a fundamental
> floor. gen3 is a fresh redo of the same in-order pipeline to clear it. This
> tree and doc are retained as the historical record. See
> [`doc/internals/penumbra2/overview.md`](../../../doc/internals/penumbra2/overview.md).

gen2.5 is the gen2 pipelined core with the textbook performance features
added (branch prediction first, then operand forwarding + regfile
write-through, then a store buffer). It is built by **composition**, not by
parameterizing gen2: gen2 stays a machine that has no notion of these
mechanisms, readable as its own design. See the rationale in
[`doc/internals/penumbra2/overview.md`](../../../doc/internals/penumbra2/overview.md)
(gen2.5 organization: composition).

## What lives here

This directory holds **only what differs from gen2** — the forked
integration files and the gen2.5-only leaf modules:

- `penumbra2_core.sv`, `penumbra2_spine.sv`, `penumbra2_id_stage.sv`,
  `penumbra2_ex_stage.sv`, `penumbra2_if2_stage.sv` — forks of the
  same-named files in `hw/rtl/penumbra2/`. They keep the same module names
  and the same
  sub-instantiation names as their gen2 originals, so they differ only by
  the gen2.5 wiring. **`diff hw/rtl/penumbra2/<f>.sv hw/rtl/penumbra2_5/<f>.sv`
  shows exactly the gen2.5 delta** — keep that diff minimal so a shared
  correctness fix ports across as a small, obvious change.
- gen2.5-only leaves (e.g. a branch predictor) land here too, instantiated
  by the forked integration files above.

Everything else — the ALU, regfile, scoreboard, decoder, the MMU/TLB/cache
stack, the unchanged stages — is **shared from `hw/rtl/penumbra2/`** and not
duplicated here. A bug fixed in a shared cell serves both generations at
once.

## How the build selects it (`CORE=penumbra2_5`)

- **Sim (`make test` / `simulate-rtl` / `benchmark-rtl`):** Verilator
  resolves modules by name from its `-I` library path. For a sub-variant
  the Makefile prepends `-Ihw/rtl/penumbra2_5`, so any module present here
  shadows its `penumbra2/` namesake; everything not here resolves from
  `penumbra2/`. No fork list to maintain — search order does the override.
- **FPGA (`make fpga`):** the fileset is explicit, so
  `SRC_CORE_penumbra2_5` is `SRC_CORE_penumbra2` with the forked files
  filtered out, plus this directory's files. The `PENUMBRA2_FORKED` list in
  the Makefile names which gen2 files are overridden; omit one and the
  build hard-errors with a duplicate-module definition (the same name in
  both directories), so the list cannot silently drift.

The shared machine (`hw/rtl/machine/machine_penumbra2.sv`) picks the cpuid
name ("Penumbra/2.5") from the build's `PENUMBRA_CPU_VARIANT` define — that
is identity, separate from the structural fork the fileset selects.

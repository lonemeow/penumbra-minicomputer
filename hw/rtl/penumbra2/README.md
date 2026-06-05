# Penumbra/2 RTL

The gen2 CPU core (6-stage pipelined, hardwired control) lives here.
The design is specified under
[`doc/internals/penumbra2/`](../../../doc/internals/penumbra2/) — read
[`pipeline-stages.md`](../../../doc/internals/penumbra2/pipeline-stages.md)
for the stage boundaries and inter-stage register layouts the RTL
implements.

Each pipeline stage is one integration module
(`penumbra2_<stage>_stage.sv`) wired from smaller combinational
submodules (`penumbra2_alu`, `penumbra2_decode`, `penumbra2_regmap`,
`penumbra2_scoreboard`, `penumbra2_flag_bypass`). `penumbra2_regfile`
is shared across stages: ID drives its read ports, WB owns the write
port. Every module has a `tb_penumbra2_<name>.cpp` Verilator
testbench registered in the Makefile's `MODULE_TESTS`.

The front end is `penumbra2_if1_stage` (PC register + fetch-address
generation) and `penumbra2_if2_stage` (deliver the fetched word + PC
to ID). `penumbra2_core` wires the front end onto the datapath:
`PC → IF1 → instruction memory → IF2 → penumbra2_spine`, closing the
back-pressure chain up to the PC and halting on a retiring BREAK. The
instruction memory is `bram_mem` (in `hw/rtl/sim/`), a flat
registered-read BRAM stand-in with a read clock-enable — the streaming
contract the IF1/IF2 split is built around, ahead of the real
BRAM-backed I-cache. `penumbra2_core` runs an assembled program rather
than a hand-driven stream, so it is exercised by `make test-penumbra2`
(which loads `hw/sim/programs/penumbra2_smoke.s` into the i-mem) rather
than `MODULE_TESTS`; IF1/IF2 are covered through it.

Not yet wired in the core: the taken-branch PC redirect (resolved in
EX but not fed back to IF1 yet), the I-side fault path (no MMU), and
loads/stores (the MEM data path is still a skeleton).

Core-internal microarchitectural constants (scoreboard indices,
`op_class` / `alu_op` / `mem_op` encodings) live in
[`penumbra2_pkg.sv`](penumbra2_pkg.sv); the ISA-level constants shared
with the rest of the machine stay in
[`hw/rtl/common/penumbra_pkg.sv`](../common/penumbra_pkg.sv) and are
used by both generations. The gen1 core is in
[`hw/rtl/penumbra1/`](../penumbra1/). The surrounding system
(`mmu/`, `soc/`, `io/`, `bus/`) is shared unchanged.

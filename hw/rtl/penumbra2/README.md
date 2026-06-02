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

Core-internal microarchitectural constants (scoreboard indices,
`op_class` / `alu_op` / `mem_op` encodings) live in
[`penumbra2_pkg.sv`](penumbra2_pkg.sv); the ISA-level constants shared
with the rest of the machine stay in
[`hw/rtl/common/penumbra_pkg.sv`](../common/penumbra_pkg.sv) and are
used by both generations. The gen1 core is in
[`hw/rtl/penumbra1/`](../penumbra1/). The surrounding system
(`mmu/`, `soc/`, `io/`, `bus/`) is shared unchanged.

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
back-pressure chain up to the PC, the taken-branch redirect from EX
back to the PC, and halting on a retiring BREAK. Fetch and data both go
through `unified_mem` (in `hw/rtl/sim/`), a dual-port registered-read
stand-in with a read clock-enable: one backing array, one address space
(ROM region for the program, RAM region for data), so a store is visible
to a later fetch — the streaming contract the IF1/IF2 split and the MEM
single-STALL are built around, ahead of the real BRAM-backed L1 caches.
`penumbra2_core` runs an assembled program rather than a hand-driven
stream, so it is exercised by `make test-penumbra2` (straight-line +
RAW-stall), `make test-penumbra2-branch` (taken/not-taken/unconditional
branches + a backward loop), and `make test-penumbra2-loadstore`
(load/store round-trips) rather than `MODULE_TESTS`; IF1/IF2 are covered
through them.

When EX resolves a branch taken it drives `o_branch_taken` /
`o_branch_target`; the core steers the PC to the target via
`if1.i_redirect` and bubbles the three wrong-path slots (IF1/IF2 via
`i_redirect`/`i_flush`, ID via the spine's `ex_branch_taken` bubble) —
the 3-bubble flush. Loads/stores run through the MEM data path; RDSYS reads
CPU-internal sysreg devices (cpuid/machid) through the MEM sideband, and WRSYS
writes them at the EX drain-commit. Still not wired in the core: the I-side
fault path (no MMU) and the real BRAM-backed L1 caches.

Core-internal microarchitectural constants (scoreboard indices,
`op_class` / `alu_op` / `mem_op` encodings) live in
[`penumbra2_pkg.sv`](penumbra2_pkg.sv); the ISA-level constants shared
with the rest of the machine stay in
[`hw/rtl/common/penumbra_pkg.sv`](../common/penumbra_pkg.sv) and are
used by both generations. The gen1 core is in
[`hw/rtl/penumbra1/`](../penumbra1/). The surrounding system
(`mmu/`, `soc/`, `io/`, `bus/`) is shared unchanged.

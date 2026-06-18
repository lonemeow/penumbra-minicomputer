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
generation) and `penumbra2_if2_stage` (request the fetched word from
the I-side front port, deliver word + PC to ID). `penumbra2_core` is
the *bare core*: it wires the front end onto `penumbra2_spine`
(closing the back-pressure chain up to the PC and the taken-branch
redirect from EX), muxes the vector-fetch FSM onto the fetch port,
and exposes everything memory-shaped as ports — the fetch and dmem
front ports (launch / level-held request / busy-drop completion), the
MMU query/verdict ports for both sides, and the sysreg sideband.

The *machine* binds those ports to the real memory system:
[`hw/rtl/machine/machine_penumbra2.sv`](../machine/machine_penumbra2.sv)
instantiates the core, `penumbra2_mmu`, two `cache_bram_vipt` instances
(VIPT, vaddr-indexed, paddr-tagged — non-identity translations work),
the transactional I/D arbiter (`txn_arbiter`), the line-fill
sequencer (`fill_sequencer`), and the shared L2, and exposes the
external Penumbra Bus plus a program-end pulse. The program runners
build `machine_penumbra2_sim` (machine + `unified_bus_mem`, both in
`hw/rtl/sim/`): `make test CORE=penumbra2`, one program via
`make test-prog CORE=penumbra2 PROG=test_<name>`. The generic runner
is `tb_penumbra2_prog` (stop on the program-end pulse, check R1);
programs needing bespoke stimulus name their testbench in a
`; RUNNER:` header tag (`test_intr` → `tb_penumbra2_intr`). Untagged
`isa/` conformance programs run on this machine too — those whose
`; REQUIRES:` capabilities it cannot provide yet (uart, timer, wrspr)
are reported as skipped (see `doc/internals/build-system.md`).

When EX resolves a branch taken it drives `o_branch_taken` /
`o_branch_target`; the core steers the PC to the target via
`if1.i_redirect` and bubbles the three wrong-path slots (IF1/IF2 via
`i_redirect`/`i_flush`, ID via the spine's `ex_branch_taken` bubble) —
the 3-bubble flush. A redirect whose target lands while the I-side is
mid-transaction steers the PC immediately but holds the launch until
the busy drop. Loads/stores run through the MEM data path against the
D-L1; RDSYS reads the machine's sysreg device complex (MMU, both L1s,
L2, cpuid) through the MEM sideband, and WRSYS writes it at the EX
drain-commit.

Core-internal microarchitectural constants (scoreboard indices,
`op_class` / `alu_op` / `mem_op` encodings) live in
[`penumbra2_pkg.sv`](penumbra2_pkg.sv); the ISA-level constants shared
with the rest of the machine stay in
[`hw/rtl/common/penumbra_pkg.sv`](../common/penumbra_pkg.sv) and are
used by both generations. The gen1 core is in
[`hw/rtl/penumbra1/`](../penumbra1/). The surrounding system
(`mmu/`, `soc/`, `io/`, `bus/`) is shared unchanged.

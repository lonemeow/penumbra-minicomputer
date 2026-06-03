// Verilator testbench for penumbra2_wb_stage (commit point).
//
// Drives the MEM/WB input and checks the architectural write strobes, the
// scoreboard destination exposure, and the dual-write sequencing against
// the WB-stage contract in doc/internals/penumbra2/{pipeline-stages,
// regfile,hazard-model}.md:
//   - reset / a bubble drives no writes
//   - a normal GPR commit drives the regfile write port + exposes its dst
//   - an SPR / flag-only commit drives only its own strobe
//   - a dual-destination write sequences primary-then-aux through the one
//     write port over two cycles, asserts o_stall on the first, and holds
//     both scoreboard exposures across both cycles (they drop together)
//
// The deferred-fault guard (no faulting instruction may retire here) is an
// `always_comb assert`; a failed $error aborts the sim (exit 1). Run
// `./Vpenumbra2_wb_stage +guard` to drive a faulting commit and watch it
// abort; the default run stays clean so the module-test exit code reflects
// only the check() failures.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_wb_stage.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_wb_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Reset the per-cycle inputs to a quiet baseline: a bubble, no writes.
static void clear(Vpenumbra2_wb_stage* dut) {
    dut->i_gpr_we = 0; dut->i_spr_we = 0; dut->i_flag_we = 0; dut->i_spr_sel = 0;
    dut->i_wb_value = 0; dut->i_wb_value_aux = 0; dut->i_flag_value = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_en = 0;
    dut->i_valid = 0; dut->i_fault_pending = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool run_guard = Verilated::commandArgsPlusMatch("guard")[0] != '\0';
    Vpenumbra2_wb_stage* dut = new Vpenumbra2_wb_stage;

    // ── Reset + idle: no writes, no stall ────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_wr_en",     dut->o_wr_en, 0);
    check("reset_stall",     dut->o_stall, 0);
    check("reset_wb_dst_en", dut->o_wb_dst_en, 0);
    check("reset_aux_en",    dut->o_aux_dst_en, 0);

    // ── Normal GPR commit: write port + dst exposure, no stall ───
    clear(dut);
    dut->i_valid = 1; dut->i_gpr_we = 1;
    dut->i_phys_dst = 7; dut->i_wb_value = 0xCAFE0000;
    dut->i_flag_we = 1; dut->i_flag_value = 0x5;
    dut->eval();
    check("gpr_wr_en",     dut->o_wr_en, 1);
    check("gpr_wr_idx",    dut->o_wr_idx, 7);
    check("gpr_wr_data",   dut->o_wr_data, 0xCAFE0000);
    check("gpr_flag_we",   dut->o_flag_we, 1);
    check("gpr_flag_val",  dut->o_flag_value, 0x5);
    check("gpr_wb_dst",    dut->o_wb_dst, 7);
    check("gpr_wb_dst_en", dut->o_wb_dst_en, 1);
    check("gpr_aux_en",    dut->o_aux_dst_en, 0);
    check("gpr_spr_we",    dut->o_spr_we, 0);
    check("gpr_no_stall",  dut->o_stall, 0);
    // A single write must not arm a phantom second cycle.
    tick(dut); clear(dut); dut->eval();
    check("gpr_no_second_write", dut->o_wr_en, 0);
    check("gpr_no_second_stall", dut->o_stall, 0);

    // ── SPR commit: only the SPR strobe ──────────────────────────
    clear(dut);
    dut->i_valid = 1; dut->i_spr_we = 1; dut->i_spr_sel = 3;
    dut->i_wb_value = 0xDEADBEEF;
    dut->eval();
    check("spr_we",     dut->o_spr_we, 1);
    check("spr_sel",    dut->o_spr_sel, 3);
    check("spr_value",  dut->o_spr_value, 0xDEADBEEF);
    check("spr_wr_en",  dut->o_wr_en, 0);
    check("spr_wb_en",  dut->o_wb_dst_en, 0);

    // ── Flag-only commit (e.g. CMP): only the flag strobe ────────
    clear(dut);
    dut->i_valid = 1; dut->i_flag_we = 1; dut->i_flag_value = 0xA;
    dut->eval();
    check("cmp_flag_we",  dut->o_flag_we, 1);
    check("cmp_flag_val", dut->o_flag_value, 0xA);
    check("cmp_wr_en",    dut->o_wr_en, 0);
    check("cmp_spr_we",   dut->o_spr_we, 0);

    // ── Bubble: write-enables set but valid=0 → no writes ────────
    clear(dut);
    dut->i_valid = 0; dut->i_gpr_we = 1; dut->i_phys_dst = 5; dut->i_flag_we = 1;
    dut->eval();
    check("bubble_wr_en",  dut->o_wr_en, 0);
    check("bubble_wb_en",  dut->o_wb_dst_en, 0);
    check("bubble_flag_we", dut->o_flag_we, 0);

    // ── Dual-destination write: primary→aux over two cycles ──────
    // MEM is back-pressured by o_stall, so the MEM/WB register is held —
    // emulate that by keeping the inputs stable across both cycles.
    clear(dut);
    dut->i_valid = 1; dut->i_gpr_we = 1;
    dut->i_phys_dst = 1;    dut->i_wb_value    = 0x11112222;  // primary (Rd / low)
    dut->i_phys_dst_aux = 2; dut->i_wb_value_aux = 0x33334444;  // aux (Rdh / high)
    dut->i_phys_dst_aux_en = 1;
    dut->i_flag_we = 1; dut->i_flag_value = 0x3;
    // Cycle 1 — primary write, hold MEM, both dsts exposed.
    dut->eval();
    check("dual_lo_wr_en",   dut->o_wr_en, 1);
    check("dual_lo_wr_idx",  dut->o_wr_idx, 1);
    check("dual_lo_wr_data", dut->o_wr_data, 0x11112222);
    check("dual_lo_stall",   dut->o_stall, 1);
    check("dual_lo_wb_dst",  dut->o_wb_dst, 1);
    check("dual_lo_wb_en",   dut->o_wb_dst_en, 1);
    check("dual_lo_aux_dst", dut->o_aux_dst, 2);
    check("dual_lo_aux_en",  dut->o_aux_dst_en, 1);
    // Cycle 2 — aux write, release; both dsts still exposed.
    tick(dut); dut->eval();
    check("dual_hi_wr_en",   dut->o_wr_en, 1);
    check("dual_hi_wr_idx",  dut->o_wr_idx, 2);
    check("dual_hi_wr_data", dut->o_wr_data, 0x33334444);
    check("dual_hi_release", dut->o_stall, 0);
    check("dual_hi_wb_en",   dut->o_wb_dst_en, 1);
    check("dual_hi_aux_en",  dut->o_aux_dst_en, 1);
    // Cycle 3 — the dual write leaves WB; both exposures drop together.
    tick(dut); clear(dut); dut->eval();
    check("dual_done_wr_en", dut->o_wr_en, 0);
    check("dual_done_wb_en", dut->o_wb_dst_en, 0);
    check("dual_done_aux_en", dut->o_aux_dst_en, 0);
    check("dual_done_stall", dut->o_stall, 0);

    // ── Deferred-fault guard demo (opt-in, aborts the sim) ───────
    if (run_guard) {
        printf("  [+guard] driving a faulting commit — expect the guard to abort:\n");
        clear(dut); dut->i_valid = 1; dut->i_fault_pending = 1;
        dut->eval();
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

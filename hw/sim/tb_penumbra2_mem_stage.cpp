// Verilator testbench for penumbra2_mem_stage (pass-through skeleton).
//
// Drives the EX/MEM input and the pipeline handshake across clock edges,
// checking the MEM/WB register and back-pressure against the MEM-stage
// contract in doc/internals/penumbra2/pipeline-stages.md:
//   - reset clears valid
//   - an ALU op latches its result + flags + ctrl into MEM/WB, no stall
//   - the divmul two-half payload (lo/hi values + Rd/Rdh) passes through
//   - a WRSPR's value reaches WB on the shared o_wb_value datum
//   - a faulting slot advances with its fault tag intact (WB suppresses)
//   - a downstream stall holds MEM/WB; release advances the held op
//   - i_bubble flushes the MEM/WB slot, and wins over a concurrent stall
//
// The deferred-path guard (no load/store/RDSYS may reach the skeleton) is
// an `always_comb assert`, so it cannot be observed through a port — and a
// failed `$error` assertion aborts the sim (exit 1). Run
// `./Vpenumbra2_mem_stage +guard` to drive a load and watch it abort; the
// default (no-plusarg) run stays clean so the automated module-test exit
// code reflects only the check() failures.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_mem_stage.h"
#include "verilated.h"

// op_class / mem_op (penumbra2_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_WRSPR = 7, OPC_RDSYS = 8 };
enum { MEM_NONE = 0, MEM_LOAD = 1, MEM_STORE = 2 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_mem_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Reset the per-cycle inputs to a quiet baseline: a valid-but-bubble ALU
// slot, no memory op, no writes, no stall/flush.
static void clear(Vpenumbra2_mem_stage* dut) {
    dut->i_op_class = OPC_ALU; dut->i_mem_op = MEM_NONE;
    dut->i_gpr_we = 0; dut->i_spr_we = 0; dut->i_flag_we = 0; dut->i_spr_sel = 0;
    dut->i_result = 0; dut->i_result_aux = 0; dut->i_flag_value = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_en = 0;
    dut->i_pc = 0; dut->i_valid = 0; dut->i_fault_pending = 0; dut->i_fault_vec = 0;
    dut->i_stall_in = 0; dut->i_bubble = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool run_guard = Verilated::commandArgsPlusMatch("guard")[0] != '\0';
    Vpenumbra2_mem_stage* dut = new Vpenumbra2_mem_stage;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_valid_low", dut->o_valid, 0);

    // ── ALU op: result + flags + ctrl latch into MEM/WB ──────────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1; dut->i_flag_we = 1;
    dut->i_result = 0xCAFEBABE; dut->i_flag_value = 0x5;   // N=1 C=1
    dut->i_phys_dst = 7; dut->i_pc = 0xFFFF0100; dut->i_valid = 1;
    dut->eval();
    check("alu_no_stall",   dut->o_stall, 0);
    tick(dut); dut->eval();
    check("alu_valid",      dut->o_valid, 1);
    check("alu_gpr_we",     dut->o_gpr_we, 1);
    check("alu_flag_we",    dut->o_flag_we, 1);
    check("alu_spr_we",     dut->o_spr_we, 0);
    check("alu_wb_value",   dut->o_wb_value, 0xCAFEBABE);
    check("alu_flag_value", dut->o_flag_value, 0x5);
    check("alu_phys_dst",   dut->o_phys_dst, 7);
    check("alu_dst_aux_en",  dut->o_phys_dst_aux_en, 0);
    check("alu_pc",         dut->o_pc, 0xFFFF0100);
    check("alu_fault",      dut->o_fault_pending, 0);
    check("alu_op_class",   dut->o_op_class, OPC_ALU);

    // ── divmul: both writeback halves + Rd/Rdh pass through ──────
    clear(dut);
    dut->i_gpr_we = 1;
    dut->i_result = 0x11112222; dut->i_result_aux = 0x33334444;
    dut->i_phys_dst = 1; dut->i_phys_dst_aux = 2; dut->i_phys_dst_aux_en = 1;
    dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("dm_wb_lo",     dut->o_wb_value, 0x11112222);
    check("dm_wb_hi",     dut->o_wb_value_aux, 0x33334444);
    check("dm_phys_dst",  dut->o_phys_dst, 1);
    check("dm_dst_aux",    dut->o_phys_dst_aux, 2);
    check("dm_dst_aux_en", dut->o_phys_dst_aux_en, 1);

    // ── WRSPR: value reaches WB on the shared o_wb_value datum ────
    clear(dut);
    dut->i_op_class = OPC_WRSPR; dut->i_spr_we = 1; dut->i_spr_sel = 3;
    dut->i_result = 0xDEADBEEF; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("wrspr_spr_we",   dut->o_spr_we, 1);
    check("wrspr_spr_sel",  dut->o_spr_sel, 3);
    check("wrspr_wb_value", dut->o_wb_value, 0xDEADBEEF);
    check("wrspr_gpr_we",   dut->o_gpr_we, 0);
    check("wrspr_op_class", dut->o_op_class, OPC_WRSPR);   // op_class carried to retire

    // ── Faulting slot advances with its fault tag intact ─────────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1; dut->i_phys_dst = 5;
    dut->i_valid = 1; dut->i_fault_pending = 1; dut->i_fault_vec = 10;
    dut->eval(); tick(dut); dut->eval();
    check("fault_valid",  dut->o_valid, 1);
    check("fault_flag",   dut->o_fault_pending, 1);
    check("fault_vec",    dut->o_fault_vec, 10);

    // ── Back-pressure: latch op#1, stall holds it past op#2 ──────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1;
    dut->i_result = 0xAAAA0000; dut->i_phys_dst = 7; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("bp_latch_dst", dut->o_phys_dst, 7);

    clear(dut);                                  // present op#2 under stall
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1;
    dut->i_result = 0xBBBB0000; dut->i_phys_dst = 9; dut->i_valid = 1;
    dut->i_stall_in = 1;
    dut->eval();
    check("bp_stall_out", dut->o_stall, 1);
    tick(dut); dut->eval();
    check("bp_hold_valid", dut->o_valid, 1);
    check("bp_hold_dst",   dut->o_phys_dst, 7);          // op#1 still held
    check("bp_hold_value", dut->o_wb_value, 0xAAAA0000);

    dut->i_stall_in = 0;                          // release: op#2 advances
    dut->eval();
    check("bp_release_out", dut->o_stall, 0);
    tick(dut); dut->eval();
    check("bp_release_dst",   dut->o_phys_dst, 9);
    check("bp_release_value", dut->o_wb_value, 0xBBBB0000);

    // ── i_bubble flushes the MEM/WB slot ─────────────────────────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1;
    dut->i_result = 0xCCCC0000; dut->i_phys_dst = 11; dut->i_valid = 1;
    dut->i_bubble = 1;
    dut->eval();
    check("bubble_stall_out", dut->o_stall, 0);
    tick(dut); dut->eval();
    check("bubble_flush_valid", dut->o_valid, 0);

    // ── i_bubble wins over a concurrent stall ────────────────────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_valid = 1; dut->i_phys_dst = 12;
    dut->i_bubble = 1; dut->i_stall_in = 1;
    dut->eval();
    check("bubble_over_stall_out", dut->o_stall, 1);     // reflects i_stall_in
    tick(dut); dut->eval();
    check("bubble_over_stall_valid", dut->o_valid, 0);

    // ── Deferred-path guard demo (opt-in, aborts the sim) ────────
    // A failed `$error` assertion stops the simulation, so this drives
    // one deferred op (a load) and the run aborts with a non-zero exit —
    // the visible proof the guard fires. Stores and RDSYS trip the same
    // predicate (i_mem_op != MEM_NONE, or i_op_class == OPC_RDSYS); swap
    // the drive below to check those in isolation.
    if (run_guard) {
        printf("  [+guard] driving a load — expect the guard to abort the sim:\n");
        clear(dut); dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_valid = 1;
        dut->eval();
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

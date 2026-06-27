// Verilator testbench for penumbra3_wb_stage (via penumbra3_wb_stage_test).
//
// WB is the commit point. The wrapper presents the bundle fields WB reads plus
// the resolved payload flat, so this drives committing slots and checks the
// write strobes WB fans to the external regfile / SPR-file / scratch storage:
//   - a normal GPR commit drives the regfile port
//   - a flag-updating commit also strobes NZCV; a CMP (no GPR dst) strobes only
//     the flags
//   - WRSPR routes by physical index: EPC/ESR/SCRn to the SPR port, but USP
//     (SPR-named, regfile-backed) to the regfile port
//   - a carried fault gates every architectural write off and pulses the
//     fault-commit descriptor
//   - a divmul dual write sequences lo then hi through the single regfile port
//     over two cycles, holding MEM2 for the second and retiring exactly once
//   - a bubble forwards its carried stall cause

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_wb_stage_test.h"
#include "verilated.h"

// Physical scoreboard indices (penumbra3_pkg)
enum { SB_USP = 14, SB_SSP = 15, SB_ESR = 16, SB_EPC = 17, SB_SCR0 = 18 };
// SPR numbers (penumbra_pkg)
enum { SPR_ESR = 0, SPR_EPC = 1, SPR_USP = 2, SPR_SR = 3, SPR_SCR0 = 4 };
// Fault vector + stall causes
enum { VEC_ARITH = 10 };
enum { BCAUSE_NONE = 0, BCAUSE_FUNIT = 5, BCAUSE_HAZARD = 6 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_wb_stage_test* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Reset every driven input to a benign idle (a bubble, no writes, no fault).
static void clear(Vpenumbra3_wb_stage_test* dut) {
    dut->i_valid = 0; dut->i_bcause = BCAUSE_NONE;
    dut->i_dst_sel = 0; dut->i_flags_updater = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_we = 0; dut->i_value = 0; dut->i_flags = 0;
    dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_we = 0; dut->i_value_aux = 0;
    dut->i_pc = 0; dut->i_fault_pending = 0; dut->i_fault_vec = 0;
    dut->i_fault_vaddr = 0; dut->i_fault_status = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_wb_stage_test* dut = new Vpenumbra3_wb_stage_test;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_committed", dut->o_insn_committed, 0);
    check("rst_stall",     dut->o_local_stall, 0);

    // ── Normal GPR commit ────────────────────────────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1; dut->i_value = 0xCAFE;
    dut->eval();
    check("gpr_we",        dut->o_regfile_we, 1);
    check("gpr_idx",       dut->o_regfile_idx, 1);
    check("gpr_data",      dut->o_regfile_data, 0xCAFE);
    check("gpr_no_spr",    dut->o_spr_we, 0);
    check("gpr_no_flag",   dut->o_flag_we, 0);
    check("gpr_no_fault",  dut->o_fault_commit, 0);
    check("gpr_committed", dut->o_insn_committed, 1);

    // ── GPR commit that also writes NZCV ─────────────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = 2; dut->i_phys_dst_we = 1; dut->i_value = 0x5;
    dut->i_flags_updater = 1; dut->i_flags = 0xA;
    dut->eval();
    check("alu_gpr_we",   dut->o_regfile_we, 1);
    check("alu_flag_we",  dut->o_flag_we, 1);
    check("alu_flag_val", dut->o_flag_value, 0xA);

    // ── CMP: flags only, no GPR write ────────────────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst_we = 0; dut->i_flags_updater = 1; dut->i_flags = 0x4;
    dut->eval();
    check("cmp_no_gpr",    dut->o_regfile_we, 0);
    check("cmp_no_spr",    dut->o_spr_we, 0);
    check("cmp_flag_we",   dut->o_flag_we, 1);
    check("cmp_flag_val",  dut->o_flag_value, 0x4);
    check("cmp_committed", dut->o_insn_committed, 1);

    // ── WRSPR -> SPR-file (ESR) ──────────────────────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = SB_ESR; dut->i_phys_dst_we = 1;
    dut->i_dst_sel = SPR_ESR; dut->i_value = 0x1111;
    dut->eval();
    check("esr_spr_we",  dut->o_spr_we, 1);
    check("esr_spr_sel", dut->o_spr_sel, SPR_ESR);
    check("esr_spr_val", dut->o_spr_value, 0x1111);
    check("esr_no_gpr",  dut->o_regfile_we, 0);

    // ── WRSPR -> scratch (SCR0); WB emits the same SPR port ──────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = SB_SCR0; dut->i_phys_dst_we = 1;
    dut->i_dst_sel = SPR_SCR0; dut->i_value = 0x2222;
    dut->eval();
    check("scr_spr_we",  dut->o_spr_we, 1);
    check("scr_spr_sel", dut->o_spr_sel, SPR_SCR0);
    check("scr_spr_val", dut->o_spr_value, 0x2222);
    check("scr_no_gpr",  dut->o_regfile_we, 0);

    // ── WRSPR USP: SPR-named but regfile-backed ──────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = SB_USP; dut->i_phys_dst_we = 1;
    dut->i_dst_sel = SPR_USP; dut->i_value = 0x3333;
    dut->eval();
    check("usp_gpr_we",  dut->o_regfile_we, 1);
    check("usp_gpr_idx", dut->o_regfile_idx, SB_USP);
    check("usp_gpr_val", dut->o_regfile_data, 0x3333);
    check("usp_no_spr",  dut->o_spr_we, 0);

    // ── Fault commit gates every architectural write off ─────────
    clear(dut);
    dut->i_valid = 1; dut->i_fault_pending = 1;
    dut->i_fault_vec = VEC_ARITH; dut->i_pc = 0x4000;
    dut->i_fault_vaddr = 0xBEEF; dut->i_fault_status = 0x55;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1; dut->i_value = 0x9;
    dut->i_flags_updater = 1; dut->i_flags = 0xF;
    dut->eval();
    check("flt_commit",    dut->o_fault_commit, 1);
    check("flt_vec",       dut->o_fault_vec, VEC_ARITH);
    check("flt_pc",        dut->o_fault_pc, 0x4000);
    check("flt_vaddr",     dut->o_fault_vaddr, 0xBEEF);
    check("flt_status",    dut->o_fault_status, 0x55);
    check("flt_no_gpr",    dut->o_regfile_we, 0);
    check("flt_no_spr",    dut->o_spr_we, 0);
    check("flt_no_flag",   dut->o_flag_we, 0);
    check("flt_committed", dut->o_insn_committed, 1);   // a fault still retires the slot

    // ── divmul dual write: lo then hi over two cycles ────────────
    // Hold the inputs across the tick: the spine's freeze keeps the same divmul
    // slot at WB for both cycles, so the testbench reproduces that here.
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1; dut->i_value = 42;        // lo
    dut->i_phys_dst_aux = 2; dut->i_phys_dst_aux_we = 1; dut->i_value_aux = 0xD00D;  // hi
    dut->i_flags_updater = 1; dut->i_flags = 0x2;
    dut->eval();
    check("dm_lo_we",       dut->o_regfile_we, 1);
    check("dm_lo_idx",      dut->o_regfile_idx, 1);
    check("dm_lo_data",     dut->o_regfile_data, 42);
    check("dm_lo_stall",    dut->o_local_stall, 1);     // hold MEM2 for the hi write
    check("dm_lo_commit",   dut->o_insn_committed, 1);   // counted on the primary cycle
    check("dm_lo_flag",     dut->o_flag_we, 1);
    tick(dut); dut->eval();                              // -> aux cycle
    check("dm_hi_we",       dut->o_regfile_we, 1);
    check("dm_hi_idx",      dut->o_regfile_idx, 2);
    check("dm_hi_data",     dut->o_regfile_data, 0xD00D);
    check("dm_hi_stall",    dut->o_local_stall, 0);     // release MEM2
    check("dm_hi_no_commit",dut->o_insn_committed, 0);   // not re-counted
    check("dm_hi_bcause",   dut->o_bcause, BCAUSE_FUNIT);
    tick(dut);                                           // -> back to primary

    // ── FSM returned to the primary cycle ────────────────────────
    clear(dut);
    dut->i_valid = 1;
    dut->i_phys_dst = 3; dut->i_phys_dst_we = 1; dut->i_value = 0x7;
    dut->eval();
    check("post_dm_we",      dut->o_regfile_we, 1);
    check("post_dm_idx",     dut->o_regfile_idx, 3);
    check("post_dm_stall",   dut->o_local_stall, 0);
    check("post_dm_commit",  dut->o_insn_committed, 1);

    // ── Bubble forwards its carried stall cause ──────────────────
    clear(dut);
    dut->i_valid = 0; dut->i_bcause = BCAUSE_HAZARD;
    dut->eval();
    check("bub_no_commit", dut->o_insn_committed, 0);
    check("bub_no_gpr",    dut->o_regfile_we, 0);
    check("bub_no_fault",  dut->o_fault_commit, 0);
    check("bub_bcause",    dut->o_bcause, BCAUSE_HAZARD);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

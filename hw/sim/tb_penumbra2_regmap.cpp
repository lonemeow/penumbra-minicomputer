// Verilator testbench for penumbra2_regmap.
//
// Exercises the ISA-to-physical mapping (hazard-model.md):
//   - R0..R13 map straight through.
//   - R14 banks: USP (user) / SSP (supervisor), with cross_bank low.
//   - SPR numbers map to their physical entries (ESR/EPC/SCRn/USP); SR
//     has no scoreboard entry (NZCV forwarded, S/I drain-serialized).
//   - The aliasing case: WRSPR USP from supervisor maps to USP
//     (not SSP) and raises cross_bank — the whole point of physical
//     addressing.
//   - Enables pass through; divmul Rdh maps as a GPR.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_regmap.h"

// SPR numbers (penumbra_pkg).
enum { SPR_ESR = 0, SPR_EPC = 1, SPR_USP = 2, SPR_SR = 3,
       SPR_SCR0 = 4, SPR_SCR1 = 5, SPR_SCR2 = 6, SPR_SCR3 = 7 };
// Physical scoreboard entries (penumbra2_pkg). NZCV is not an entry.
enum { SB_USP = 14, SB_SSP = 15, SB_ESR = 16, SB_EPC = 17,
       SB_SCR0 = 18, SB_SCR1 = 19, SB_SCR2 = 20, SB_SCR3 = 21 };

static int errors = 0;
static int tests = 0;

static void check(const char* name, int got, int expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got %d, expected %d\n", name, got, expected);
        errors++;
    }
}

static void clear(Vpenumbra2_regmap* dut) {
    dut->i_supervisor = 0;
    dut->i_src_a_sel = 0; dut->i_src_a_is_spr = 0; dut->i_src_a_en = 0;
    dut->i_src_b_sel = 0; dut->i_src_b_is_spr = 0; dut->i_src_b_en = 0;
    dut->i_dst_sel = 0;   dut->i_dst_is_spr = 0;   dut->i_dst_en = 0;
    dut->i_dst_hi_sel = 0; dut->i_dst_hi_en = 0;
}

int main() {
    Vpenumbra2_regmap* dut = new Vpenumbra2_regmap;

    // ── GPR sources map straight through ─────────────────────────
    clear(dut);
    dut->i_src_a_sel = 5; dut->i_src_a_en = 1;
    dut->i_src_b_sel = 13; dut->i_src_b_en = 1;
    dut->eval();
    check("gpr_src_a_r5", dut->o_src_a, 5);
    check("gpr_src_a_en", dut->o_src_a_en, 1);
    check("gpr_src_b_r13", dut->o_src_b, 13);
    clear(dut);
    dut->i_src_a_sel = 0; dut->i_src_a_en = 1;
    dut->eval();
    check("r0_maps_zero", dut->o_src_a, 0);

    // ── R14 banking (cross_bank low for normal R14) ──────────────
    clear(dut);
    dut->i_src_a_sel = 14; dut->i_src_a_en = 1; dut->i_supervisor = 0;
    dut->eval();
    check("r14_user_usp", dut->o_src_a, SB_USP);
    check("r14_user_no_crossbank", dut->o_cross_bank, 0);
    dut->i_supervisor = 1;
    dut->eval();
    check("r14_super_ssp", dut->o_src_a, SB_SSP);
    check("r14_super_no_crossbank", dut->o_cross_bank, 0);

    // ── SPR destinations map to their entries ────────────────────
    clear(dut);
    dut->i_dst_is_spr = 1; dut->i_dst_en = 1;
    dut->i_dst_sel = SPR_ESR; dut->eval();
    check("spr_esr", dut->o_dst, SB_ESR);
    dut->i_dst_sel = SPR_EPC; dut->eval();
    check("spr_epc", dut->o_dst, SB_EPC);
    dut->i_dst_sel = SPR_SCR0; dut->eval();
    check("spr_scr0", dut->o_dst, SB_SCR0);
    dut->i_dst_sel = SPR_SCR3; dut->eval();
    check("spr_scr3", dut->o_dst, SB_SCR3);
    // SR is not scoreboarded — the decoder never drives it as an
    // enabled SPR reference, so regmap is not exercised with it here
    // (doing so trips the SR-not-a-scoreboard-ref assertion).

    // ── RDSPR USP: source maps to USP, raises cross_bank ─────────
    clear(dut);
    dut->i_src_a_is_spr = 1; dut->i_src_a_sel = SPR_USP; dut->i_src_a_en = 1;
    dut->eval();
    check("rdspr_usp_maps_usp", dut->o_src_a, SB_USP);
    check("rdspr_usp_crossbank", dut->o_cross_bank, 1);

    // ── Aliasing: WRSPR USP from supervisor → USP, not SSP ───────
    clear(dut);
    dut->i_supervisor = 1;                       // supervisor mode
    dut->i_dst_is_spr = 1; dut->i_dst_sel = SPR_USP; dut->i_dst_en = 1;
    dut->eval();
    check("wrspr_usp_super_hits_usp", dut->o_dst, SB_USP);   // NOT SB_SSP
    check("wrspr_usp_super_crossbank", dut->o_cross_bank, 1);
    // Contrast: a normal R14 write in supervisor reaches SSP, no cross.
    clear(dut);
    dut->i_supervisor = 1;
    dut->i_dst_sel = 14; dut->i_dst_en = 1;       // is_spr=0
    dut->eval();
    check("r14_write_super_hits_ssp", dut->o_dst, SB_SSP);
    check("r14_write_super_no_crossbank", dut->o_cross_bank, 0);

    // ── Enables pass through ─────────────────────────────────────
    clear(dut);
    dut->i_src_b_sel = 7;   // but en stays 0
    dut->eval();
    check("disabled_src_b_en", dut->o_src_b_en, 0);

    // ── Divmul Rdh maps as a GPR ─────────────────────────────────
    clear(dut);
    dut->i_dst_hi_sel = 9; dut->i_dst_hi_en = 1;
    dut->eval();
    check("dst_hi_maps_gpr", dut->o_dst_hi, 9);
    check("dst_hi_en", dut->o_dst_hi_en, 1);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_regmap: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

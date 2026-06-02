// Verilator testbench for penumbra2_scoreboard.
//
// Exercises the contract in doc/internals/penumbra2/hazard-model.md:
//   - Idle: every entry valid, no stall.
//   - An in-flight writer (EX/MEM/WB) makes its destination invalid;
//     a source reading it stalls, a source reading anything else
//     does not.
//   - Source-enable gating: a source that is not read never stalls.
//   - Entry 0 (R0) is tied valid regardless of writers.
//   - Re-derive / last-writer-wins: an entry stays invalid while ANY
//     writer targets it, and is valid again only when none do.
//   - The auxiliary writer (e.g. divmul's second destination) is an
//     in-flight writer too.
//   - USP (14) and SSP (15) are distinct physical entries.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_scoreboard.h"

// Scoreboard entry indices (mirror penumbra2_pkg).
enum { SB_USP = 14, SB_SSP = 15, SB_ESR = 16, SB_EPC = 17,
       SB_NZCV = 18, SB_SCR0 = 19, SB_SCR1 = 20, SB_SCR2 = 21, SB_SCR3 = 22 };

static int errors = 0;
static int tests = 0;

static void check(const char* name, int got, int expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got %d, expected %d\n", name, got, expected);
        errors++;
    }
}

// Clear all writers and sources (idle scoreboard).
static void clear(Vpenumbra2_scoreboard* dut) {
    dut->i_src_a = 0;  dut->i_src_a_en = 0;
    dut->i_src_b = 0;  dut->i_src_b_en = 0;
    dut->i_ex_dst = 0; dut->i_ex_dst_en = 0;
    dut->i_mem_dst = 0; dut->i_mem_dst_en = 0;
    dut->i_wb_dst = 0; dut->i_wb_dst_en = 0;
    dut->i_aux_dst = 0; dut->i_aux_dst_en = 0;
}

static bool valid_bit(Vpenumbra2_scoreboard* dut, int p) {
    return (dut->o_valid >> p) & 1u;
}

// Does the ID instruction reading just src_a stall?
static int stall_reading(Vpenumbra2_scoreboard* dut, int src_a) {
    dut->i_src_a = src_a;  dut->i_src_a_en = 1;
    dut->i_src_b = 0;      dut->i_src_b_en = 0;
    dut->eval();
    return dut->o_stall;
}

int main() {
    Vpenumbra2_scoreboard* dut = new Vpenumbra2_scoreboard;

    // ── Idle: all valid, no stall ────────────────────────────────
    clear(dut);
    dut->eval();
    int all_valid = 1;
    for (int p = 1; p < 23; p++) if (!valid_bit(dut, p)) all_valid = 0;
    check("idle_all_valid", all_valid, 1);
    check("idle_no_stall", dut->o_stall, 0);

    // ── EX writer invalidates its destination ────────────────────
    clear(dut);
    dut->i_ex_dst = 5; dut->i_ex_dst_en = 1;
    dut->eval();
    check("ex_dst5_invalid", valid_bit(dut, 5), 0);
    check("ex_other_valid",  valid_bit(dut, 6), 1);
    check("read_busy_stalls",  stall_reading(dut, 5), 1);
    check("read_free_no_stall", stall_reading(dut, 6), 0);

    // ── Source-enable gating ─────────────────────────────────────
    clear(dut);
    dut->i_ex_dst = 5; dut->i_ex_dst_en = 1;
    dut->i_src_a = 5;  dut->i_src_a_en = 0;   // reads nothing
    dut->eval();
    check("disabled_source_no_stall", dut->o_stall, 0);

    // ── Entry 0 (R0) tied valid even with a writer "targeting" it ─
    clear(dut);
    dut->i_ex_dst = 0; dut->i_ex_dst_en = 1;
    dut->eval();
    check("r0_tied_valid", valid_bit(dut, 0), 1);
    check("r0_read_no_stall", stall_reading(dut, 0), 0);

    // ── MEM and WB writers ───────────────────────────────────────
    clear(dut);
    dut->i_mem_dst = 7;       dut->i_mem_dst_en = 1;
    dut->i_wb_dst  = SB_NZCV; dut->i_wb_dst_en  = 1;
    dut->eval();
    check("mem_dst7_invalid",   valid_bit(dut, 7), 0);
    check("wb_nzcv_invalid",    valid_bit(dut, SB_NZCV), 0);
    check("read_mem_dst_stalls", stall_reading(dut, 7), 1);
    check("read_wb_dst_stalls",  stall_reading(dut, SB_NZCV), 1);

    // ── Both sources: stall if either hits ───────────────────────
    clear(dut);
    dut->i_mem_dst = 9; dut->i_mem_dst_en = 1;
    dut->i_src_a = 3;   dut->i_src_a_en = 1;   // free
    dut->i_src_b = 9;   dut->i_src_b_en = 1;   // busy
    dut->eval();
    check("src_b_hit_stalls", dut->o_stall, 1);

    // ── Auxiliary writer (e.g. divmul second destination) ───────
    clear(dut);
    dut->i_aux_dst = SB_SCR1; dut->i_aux_dst_en = 1;
    dut->eval();
    check("aux_dst_invalid",  valid_bit(dut, SB_SCR1), 0);
    check("read_aux_dst_stalls", stall_reading(dut, SB_SCR1), 1);

    // ── Re-derive: invalid while ANY writer targets the entry ────
    clear(dut);
    dut->i_ex_dst = 11; dut->i_ex_dst_en = 1;
    dut->i_wb_dst = 11; dut->i_wb_dst_en = 1;   // two writers, same entry
    dut->eval();
    check("two_writers_invalid", valid_bit(dut, 11), 0);
    dut->i_ex_dst_en = 0;                        // older writer drains
    dut->eval();
    check("still_invalid_youngest_remains", valid_bit(dut, 11), 0);
    dut->i_wb_dst_en = 0;                         // last writer drains
    dut->eval();
    check("valid_when_none_target", valid_bit(dut, 11), 1);

    // ── USP and SSP are distinct physical entries ────────────────
    clear(dut);
    dut->i_ex_dst = SB_USP; dut->i_ex_dst_en = 1;
    dut->eval();
    check("usp_invalid", valid_bit(dut, SB_USP), 0);
    check("ssp_still_valid", valid_bit(dut, SB_SSP), 1);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_scoreboard: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

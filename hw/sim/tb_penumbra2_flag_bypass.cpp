// Verilator testbench for penumbra2_flag_bypass.
//
// Exercises the youngest-first flag forwarding contract from
// doc/internals/penumbra2/hazard-model.md:
//   - no in-flight producer        -> committed SR
//   - only WB writes flags         -> WB
//   - only MEM writes flags        -> MEM
//   - both write flags             -> MEM wins (younger)
//   - a live producer overrides SR (SR does not leak through)
//
// The three sources carry distinct sentinel values so the selected
// one is unambiguous.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_flag_bypass.h"

static int errors = 0;
static int tests = 0;

static void check(const char* name, int got, int expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", name, got, expected);
        errors++;
    }
}

int main() {
    Vpenumbra2_flag_bypass* dut = new Vpenumbra2_flag_bypass;

    // Distinct 4-bit sentinels for each source.
    const int SR = 0x1, MEM = 0x2, WB = 0x4;

    dut->i_sr_flags  = SR;
    dut->i_mem_flags = MEM;
    dut->i_wb_flags  = WB;

    // ── No producer in flight → committed SR ─────────────────────
    dut->i_mem_writes_flags = 0; dut->i_wb_writes_flags = 0;
    dut->eval();
    check("none_uses_sr", dut->o_flags, SR);

    // ── Only WB writes → WB ──────────────────────────────────────
    dut->i_mem_writes_flags = 0; dut->i_wb_writes_flags = 1;
    dut->eval();
    check("wb_only_uses_wb", dut->o_flags, WB);

    // ── Only MEM writes → MEM ────────────────────────────────────
    dut->i_mem_writes_flags = 1; dut->i_wb_writes_flags = 0;
    dut->eval();
    check("mem_only_uses_mem", dut->o_flags, MEM);

    // ── Both write → MEM wins (younger) ──────────────────────────
    dut->i_mem_writes_flags = 1; dut->i_wb_writes_flags = 1;
    dut->eval();
    check("both_mem_wins", dut->o_flags, MEM);

    // ── A live producer overrides SR (no SR leak) ────────────────
    dut->i_sr_flags = 0xF;
    dut->i_mem_writes_flags = 0; dut->i_wb_writes_flags = 1;
    dut->eval();
    check("sr_ignored_when_wb_live", dut->o_flags, WB);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_flag_bypass: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

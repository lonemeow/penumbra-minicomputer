// Verilator testbench for penumbra2_core running the branch-redirect test.
//
// Companion to tb_penumbra2_core.cpp (straight-line first-light). This one
// loads penumbra2_branch.s, which exercises the taken-branch PC redirect and
// the 3-bubble front-end flush that closes the fetch loop. The program is
// self-checking: it sets R1=1 only if every taken branch both steered the PC
// to its target and discarded the wrong-path fetches in its shadow. A broken
// redirect or a leaked flush instead lands on a poison "LLI R1,#0 / BREAK",
// so the test fails by either R1=0 or an early poison-BREAK halt.
//
// "Stop on BREAK" is testbench policy, not core behavior (BREAK is a trap on
// real hardware). A flushed BREAK is a bubble and never retires, so only the
// program's real final BREAK trips the halt — which is itself part of what the
// flush has to get right.
//
// Run: make sim MOD=penumbra2_core PROG=penumbra2_branch TB=tb_penumbra2_branch

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_core.h"

enum { OPC_BREAK = 14 };   // penumbra2_pkg OPC_BREAK

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_core* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Vpenumbra2_core* dut = new Vpenumbra2_core;
    uint32_t shadow[22] = {0};   // committed values, indexed by physical entry

    // Reset, held two cycles (testbench convention).
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // Run until the core halts on a *retiring* BREAK, with a safety cap. The
    // cap is generous: the loop adds 3 flush bubbles per taken backward branch
    // plus scoreboard RAW stalls, so the program is a few hundred cycles.
    const int CYCLE_CAP = 4000;
    bool halted = false;
    for (int c = 0; c < CYCLE_CAP && !halted; c++) {
        dut->eval();
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        halted = dut->o_retire_valid && (dut->o_retire_op_class == OPC_BREAK);
        tick(dut);
    }

    if (!halted) { printf("  FAIL: core did not halt within %d cycles\n", CYCLE_CAP); errors++; tests++; }

    // R1 == 1 is reachable only if every branch redirect and flush behaved.
    check("R1 (PASS flag)", shadow[1], 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

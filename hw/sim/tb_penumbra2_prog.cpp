// Verilator testbench for penumbra2_core — the generic gen2 program runner.
//
// Loads a self-checking program (+rom_hex=), runs it until a BREAK retires,
// and checks the repo-wide hw-test convention: R1 == 1 is PASS. The WB commit
// port (o_commit_*) is mirrored into a shadow register file so the final
// architectural state is visible without a debug port.
//
// "Stop on BREAK" is testbench policy, not core behavior (BREAK is a trap on
// real hardware). A flushed BREAK is a bubble and never retires, so only a
// BREAK the program really reaches trips the halt — a leaked wrong-path BREAK
// failing here is part of what the front-end flush has to get right.
//
// Programs that need bespoke stimulus (e.g. driving the IRQ line) name their
// runner with a "; RUNNER: tb_<name>" header tag instead of this default.
//
// Run: make test-prog CORE=penumbra2 PROG=test_<name>

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

    // Reset, held two cycles (testbench convention). IRQ lines idle.
    dut->i_irq = 0; dut->i_timer_irq = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // Run until the core halts on a *retiring* BREAK, with a safety cap
    // generous enough for flush bubbles, scoreboard RAW stalls, and
    // multi-cycle divmul iterations.
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

// Verilator testbench for penumbra2_core — the first gen2 core that fetches
// and executes its own instructions from memory.
//
// The DUT runs a program loaded into its i-mem ($readmemh from program.hex,
// assembled by the `make sim MOD=penumbra2_core PROG=<prog>` flow). The
// testbench drives clock/reset, mirrors the WB commit port (o_commit_*) into
// a shadow register file, and stops when it sees a BREAK retire on the core's
// retire-observability outputs. It then checks the committed architectural
// state.
//
// "Stop on BREAK" is a *testbench* policy, not core behavior: real hardware
// never halts on an instruction (BREAK is a trap). The core only exposes that
// an OPC_BREAK retired; deciding that means "program done" lives here.
//
// Checking committed *values* (not cycle counts) is the real test: gen2 has
// no register write-through, so a missed scoreboard stall would let the
// RAW-dependent ADD read a stale R1 and commit the wrong R3. Correct final
// values prove fetch, decode, execute, the scoreboard stall under real
// fetch, and the halt path all work end to end.
//
// Run: make sim MOD=penumbra2_core PROG=penumbra2_smoke TB=tb_penumbra2_core

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

    // Run until the core halts on BREAK, with a generous safety cap.
    const int CYCLE_CAP = 2000;
    bool halted = false;
    for (int c = 0; c < CYCLE_CAP && !halted; c++) {
        dut->eval();
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        halted = dut->o_retire_valid && (dut->o_retire_op_class == OPC_BREAK);
        tick(dut);
    }

    if (!halted) { printf("  FAIL: core did not halt within %d cycles\n", CYCLE_CAP); errors++; tests++; }

    // Expected committed state from penumbra2_smoke.s.
    check("R1", shadow[1], 8);
    check("R2", shadow[2], 3);
    check("R3", shadow[3], 24);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

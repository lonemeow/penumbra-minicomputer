// Verilator testbench for penumbra2_core driving an external interrupt.
//
// Like tb_penumbra2_branch (mirror WB commits into a shadow regfile, stop on a
// retiring BREAK, check R1), but it holds the external IRQ line asserted from
// reset. The program (penumbra2_intr.s) must mask it until interrupts are
// enabled and the EI shadow has passed, then take it and vector to a handler
// that sets the PASS flag — so R1 == 1 proves recognition, the EI delay, the
// drain-and-take, and the vector redirect all behaved.
//
// Run: make sim MOD=penumbra2_core PROG=penumbra2_intr TB=tb_penumbra2_intr

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_core.h"

enum { OPC_BREAK = 14 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_core* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

int main() {
    Vpenumbra2_core* dut = new Vpenumbra2_core;
    uint32_t shadow[22] = {0};

    dut->i_irq = 0; dut->i_timer_irq = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // Hold the external IRQ asserted for the rest of the run; the program is
    // responsible for masking it until it is ready to take it.
    dut->i_irq = 1;

    const int CYCLE_CAP = 4000;
    bool halted = false;
    for (int c = 0; c < CYCLE_CAP && !halted; c++) {
        dut->eval();
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        halted = dut->o_retire_valid && (dut->o_retire_op_class == OPC_BREAK);
        tick(dut);
    }

    if (!halted) { printf("  FAIL: core did not halt within %d cycles\n", CYCLE_CAP); errors++; tests++; }

    check("R1 (PASS flag)", shadow[1], 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

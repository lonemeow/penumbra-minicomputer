// Verilator testbench for machine_penumbra2_sim driving an external
// interrupt.
//
// Like tb_penumbra2_prog (mirror WB commits into a shadow regfile, stop on
// the machine's program-end pulse, check R1), but it holds the external IRQ
// line asserted from reset. The program (test_intr.s, which selects this
// runner via its "; RUNNER:" tag) must mask it until interrupts are enabled
// and the EI shadow has passed, then take it and vector to a handler that
// sets the PASS flag — so R1 == 1 proves recognition, the EI delay, the
// drain-and-take, and the vector redirect all behaved.
//
// Run: make test-prog CORE=penumbra2 PROG=test_intr

#include <cstdio>
#include <cstdint>
#include "Vmachine_penumbra2_sim.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vmachine_penumbra2_sim* dut) {
    // Dual clock: 4 SDRAM half-cycles per CPU half-cycle (see tb_penumbra2_prog).
    dut->i_clk = 0; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
    dut->i_clk = 1; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Vmachine_penumbra2_sim* dut = new Vmachine_penumbra2_sim;
    uint32_t shadow[22] = {0};

    dut->i_irq = 0; dut->i_timer_irq = 0; dut->i_sdram_clk = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // Hold the external IRQ asserted for the rest of the run; the program is
    // responsible for masking it until it is ready to take it.
    dut->i_irq = 1;

    // Matches tb_penumbra2_prog: generous for the full SDRAM model's fills.
    const int CYCLE_CAP = 2000000;
    bool ended = false;
    for (int c = 0; c < CYCLE_CAP && !ended; c++) {
        dut->eval();
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        ended = dut->o_prog_end;
        tick(dut);
    }

    if (!ended) { printf("  FAIL: no program end within %d cycles\n", CYCLE_CAP); errors++; tests++; }

    check("R1 (PASS flag)", shadow[1], 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

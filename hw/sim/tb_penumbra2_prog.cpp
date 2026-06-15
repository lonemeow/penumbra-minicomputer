// Verilator testbench for machine_penumbra2_sim — the generic gen2 program
// runner.
//
// Loads a self-checking program (+rom_hex=), runs the gen2 machine until its
// program-end pulse fires, and checks the repo-wide hw-test convention:
// R1 == 1 is PASS. The WB commit port (o_commit_*) is mirrored into a shadow
// register file so the final architectural state is visible without a debug
// port.
//
// The runner keys only on the program-end contract (build-system.md): clock,
// reset, o_prog_end, R1 readback. o_prog_end is the machine's "a BREAK is
// retiring" pulse — BREAK is a trap on real hardware, not a halt; a flushed
// wrong-path BREAK is a bubble and never retires, so a leaked one failing
// here is part of what the front-end flush has to get right. In-order commit
// guarantees the shadow regfile holds final state at the pulse.
//
// Programs that need bespoke stimulus (e.g. driving the IRQ line) name their
// runner with a "; RUNNER: tb_<name>" header tag instead of this default.
//
// Run: make test-prog CORE=penumbra2 PROG=test_<name>

#include <cstdio>
#include <cstdint>
#include "Vmachine_penumbra2_sim.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vmachine_penumbra2_sim* dut) {
    // Dual clock: 4 SDRAM half-cycles per CPU half-cycle, matching hardware's
    // 25 MHz CPU / 100 MHz SDRAM ratio so memory latency in CPU cycles tracks
    // the FPGA. Each SDRAM toggle gets its own eval() so the SDRAM-domain RTL
    // (controller, CDC's sd side, chip model) advances on its own clock.
    dut->i_clk = 0; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
    dut->i_clk = 1; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Vmachine_penumbra2_sim* dut = new Vmachine_penumbra2_sim;
    uint32_t shadow[22] = {0};   // committed values, indexed by physical entry

    // Reset, held two cycles (testbench convention). IRQ lines idle.
    dut->i_irq = 0; dut->i_timer_irq = 0; dut->i_sdram_clk = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // Run until the machine pulses program end, with a safety cap generous
    // enough for uncached boot-mode fetches through the L1/arbiter/L2 path,
    // line fills, flush bubbles, multi-cycle divmul iterations, and the
    // page-walking MMU tests. Matches the gen1 runner's cap: both drive the
    // full SDRAM model, whose multi-cycle line fills dominate any test whose
    // working set thrashes the L1 (a 16 KiB walk needs well over the 500k that
    // sufficed under the old 1-cycle memory model).
    const int CYCLE_CAP = 2000000;
    bool ended = false;
    long retires = 0, last_retire = -1;
    for (int c = 0; c < CYCLE_CAP && !ended; c++) {
        dut->eval();
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        if (dut->o_retire_valid) { retires++; last_retire = c; }
        ended = dut->o_prog_end;
        tick(dut);
    }

    // On a timeout, distinguish a wedged pipeline (retires stopped long ago)
    // from a program that is merely slow or spinning (still retiring).
    if (!ended) {
        printf("  FAIL: no program end within %d cycles"
               " (%ld retires, last at cycle %ld)\n",
               CYCLE_CAP, retires, last_retire);
        errors++; tests++;
    }

    // R1 == 1 is reachable only if every branch redirect and flush behaved.
    check("R1 (PASS flag)", shadow[1], 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

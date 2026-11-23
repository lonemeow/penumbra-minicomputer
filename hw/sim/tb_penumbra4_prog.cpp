// Verilator testbench for machine_penumbra4_sim — the generic gen4 program
// runner, a fork of tb_penumbra3_prog for gen4's commit ports.
//
// Loads a self-checking program (+rom_hex=), runs the gen4 machine until its
// program-end pulse fires, and checks the repo-wide hw-test convention:
// R1 == 1 is PASS. Both commit write ports are mirrored into a shadow
// register file, indexed by physical register, so the final architectural
// state is visible without a debug port.
//
// The runner keys only on the program-end contract (build-system.md): clock,
// reset, o_prog_end, R1 readback. o_prog_end pulses as a BREAK's trap
// commits, and in-order commit guarantees the shadow file holds final state
// then.
//
// Run: make test-prog CORE=penumbra4 PROG=test_<name>

#include <cstdio>
#include <cstdint>
#include "Vmachine_penumbra4_sim.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vmachine_penumbra4_sim* dut) {
    // Four SDRAM half-cycles per CPU half-cycle, the ULX3S's 25 MHz CPU /
    // 100 MHz SDRAM ratio, so memory latency in CPU cycles tracks the board.
    dut->i_clk = 0; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
    dut->i_clk = 1; dut->eval();
    for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Vmachine_penumbra4_sim* dut = new Vmachine_penumbra4_sim;

    // Physical registers: R0..R13, then USP and SSP (penumbra4_pkg's phys_reg_t).
    const int PHYS_REGS = 16, PHYS_BITS = 4, PHYS_USP = 14, PHYS_SSP = 15;
    uint32_t shadow[PHYS_REGS] = {0};

    dut->i_irq = 0; dut->i_timer_irq = 0; dut->i_sdram_clk = 0;
    dut->i_uart_rx_valid = 0; dut->i_spi_resp_valid = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    // The cap covers uncached fetches through the whole memory stack and the
    // page-walking MMU tests, as the other generations' runners do.
    const long CYCLE_CAP = 2000000;
    bool ended = false;
    long retires = 0, last_retire = -1;
    for (long c = 0; c < CYCLE_CAP && !ended; c++) {
        dut->eval();
        for (int p = 0; p < 2; p++) {
            if (dut->o_commit_we & (1u << p)) {
                unsigned phys = (dut->o_commit_phys >> (PHYS_BITS * p)) & (PHYS_REGS - 1);
                shadow[phys] = uint32_t(dut->o_commit_data >> (32 * p));
            }
        }
        if (dut->o_retire_count) { retires += dut->o_retire_count; last_retire = c; }
        ended = dut->o_prog_end;
        tick(dut);
    }

    // A wedged machine stopped retiring long before the cap; a slow or
    // spinning program was still retiring.
    if (!ended) {
        printf("  FAIL: no program end within %ld cycles"
               " (%ld retires, last at cycle %ld)\n",
               CYCLE_CAP, retires, last_retire);
        errors++; tests++;
    }

    check("R1 (PASS flag)", shadow[1], 1);

    if (errors) {
        printf("  shadow registers at BREAK:\n");
        for (int i = 0; i < PHYS_USP; i++)
            printf("    R%-2d = 0x%08X\n", i, shadow[i]);
        printf("    USP = 0x%08X\n", shadow[PHYS_USP]);
        printf("    SSP = 0x%08X\n", shadow[PHYS_SSP]);
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

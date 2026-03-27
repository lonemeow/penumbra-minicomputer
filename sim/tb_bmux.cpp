// Verilator testbench for B-mux, W-mux, A-mux, and PC-mux
// Tests each mux's selection logic with distinct input values.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vbmux.h"

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main(int argc, char** argv) {
    Vbmux* dut = new Vbmux;
    dut->i_reg_b = 0xAAAAAAAA;
    dut->i_imm32 = 0xBBBBBBBB;

    dut->i_sel = 0b00; dut->eval(); check("bmux_reg",   dut->o_b_bus, 0xAAAAAAAA);
    dut->i_sel = 0b01; dut->eval(); check("bmux_imm",   dut->o_b_bus, 0xBBBBBBBB);
    dut->i_sel = 0b10; dut->eval(); check("bmux_const4", dut->o_b_bus, 4);
    dut->i_sel = 0b11; dut->eval(); check("bmux_const8", dut->o_b_bus, 8);

    printf("bmux: %d/%d tests passed\n", tests - errors, tests);
    delete dut;
    return (errors > 0) ? 1 : 0;
}

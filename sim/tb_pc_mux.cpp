#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vpc_mux.h"

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main(int argc, char** argv) {
    Vpc_mux* dut = new Vpc_mux;
    dut->i_pc_current = 0x00001000;
    dut->i_pc_plus4   = 0x00001004;
    dut->i_pc_offset  = 0x00001100;
    dut->i_a_bus      = 0xDEADBEEF;
    dut->i_mdr        = 0x00000040;

    dut->i_sel = 0b000; dut->eval(); check("pcmux_hold",   dut->o_pc_next, 0x00001000);
    dut->i_sel = 0b001; dut->eval(); check("pcmux_plus4",  dut->o_pc_next, 0x00001004);
    dut->i_sel = 0b010; dut->eval(); check("pcmux_offset", dut->o_pc_next, 0x00001100);
    dut->i_sel = 0b011; dut->eval(); check("pcmux_abus",   dut->o_pc_next, 0xDEADBEEF);
    dut->i_sel = 0b100; dut->eval(); check("pcmux_mdr",    dut->o_pc_next, 0x00000040);

    // Undefined selectors should hold (safe default)
    dut->i_sel = 0b111; dut->eval(); check("pcmux_default", dut->o_pc_next, 0x00001000);

    printf("pc_mux: %d/%d tests passed\n", tests - errors, tests);
    delete dut;
    return (errors > 0) ? 1 : 0;
}

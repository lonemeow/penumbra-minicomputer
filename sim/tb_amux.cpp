#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vamux.h"

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main(int argc, char** argv) {
    Vamux* dut = new Vamux;
    dut->i_reg_a       = 0xAAAA0001;
    dut->i_shadow_sr   = 0xBBBB0002;
    dut->i_shadow_pc   = 0xCCCC0003;
    dut->i_vector_addr = 0xDDDD0004;

    dut->i_sel = 0b00; dut->eval(); check("amux_reg",    dut->o_a_bus, 0xAAAA0001);
    dut->i_sel = 0b01; dut->eval(); check("amux_sr",     dut->o_a_bus, 0xBBBB0002);
    dut->i_sel = 0b10; dut->eval(); check("amux_pc",     dut->o_a_bus, 0xCCCC0003);
    dut->i_sel = 0b11; dut->eval(); check("amux_vector", dut->o_a_bus, 0xDDDD0004);

    printf("amux: %d/%d tests passed\n", tests - errors, tests);
    delete dut;
    return (errors > 0) ? 1 : 0;
}

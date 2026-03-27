#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vwmux.h"

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main(int argc, char** argv) {
    Vwmux* dut = new Vwmux;
    dut->i_r_bus = 0x11111111;
    dut->i_mdr   = 0x22222222;

    dut->i_sel = 0; dut->eval(); check("wmux_rbus", dut->o_wr_data, 0x11111111);
    dut->i_sel = 1; dut->eval(); check("wmux_mdr",  dut->o_wr_data, 0x22222222);

    printf("wmux: %d/%d tests passed\n", tests - errors, tests);
    delete dut;
    return (errors > 0) ? 1 : 0;
}

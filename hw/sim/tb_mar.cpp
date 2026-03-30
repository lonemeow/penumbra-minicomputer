// Verilator testbench for the Penumbra MAR

#include <cstdio>
#include <cstdint>
#include "Vmar.h"

static int errors = 0, tests = 0;

static void tick(Vmar* d) { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main() {
    Vmar* d = new Vmar;

    // Reset
    d->i_rst = 1; d->i_load = 0; d->i_rbus = 0;
    tick(d);
    d->i_rst = 0;
    check("reset", d->o_addr, 0);

    // Hold when load=0
    d->i_rbus = 0xDEADBEEF; d->i_load = 0;
    tick(d);
    check("hold", d->o_addr, 0);

    // Load from R-bus
    d->i_rbus = 0x12345678; d->i_load = 1;
    tick(d);
    d->i_load = 0;
    check("load", d->o_addr, 0x12345678);

    // Holds after load
    d->i_rbus = 0xFFFFFFFF;
    tick(d);
    check("hold_after_load", d->o_addr, 0x12345678);

    // Overwrite
    d->i_rbus = 0xABCD0000; d->i_load = 1;
    tick(d);
    check("overwrite", d->o_addr, 0xABCD0000);

    // Reset clears
    d->i_rst = 1;
    tick(d);
    d->i_rst = 0;
    check("reset_clears", d->o_addr, 0);

    printf("mar: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}

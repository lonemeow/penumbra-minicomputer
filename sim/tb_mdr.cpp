// Verilator testbench for the Penumbra MDR

#include <cstdio>
#include <cstdint>
#include "Vmdr.h"

static int errors = 0, tests = 0;

static void tick(Vmdr* d) { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

int main() {
    Vmdr* d = new Vmdr;

    // Reset
    d->i_rst = 1; d->i_load_mem = 0; d->i_load_a = 0;
    d->i_mem_data = 0; d->i_a_bus = 0;
    tick(d);
    d->i_rst = 0;
    check("reset", d->o_data, 0);

    // Hold when both loads=0
    d->i_mem_data = 0x11111111; d->i_a_bus = 0x22222222;
    tick(d);
    check("hold", d->o_data, 0);

    // Load from memory
    d->i_mem_data = 0xAAAABBBB; d->i_load_mem = 1;
    tick(d);
    d->i_load_mem = 0;
    check("load_mem", d->o_data, 0xAAAABBBB);

    // Load from A-bus (store path)
    d->i_a_bus = 0xCCCCDDDD; d->i_load_a = 1;
    tick(d);
    d->i_load_a = 0;
    check("load_a", d->o_data, 0xCCCCDDDD);

    // load_mem has priority over load_a (both asserted)
    d->i_mem_data = 0x11110000; d->i_a_bus = 0x22220000;
    d->i_load_mem = 1; d->i_load_a = 1;
    tick(d);
    d->i_load_mem = 0; d->i_load_a = 0;
    check("mem_priority", d->o_data, 0x11110000);

    // Holds after load
    d->i_mem_data = 0xFFFFFFFF; d->i_a_bus = 0xFFFFFFFF;
    tick(d);
    check("hold_after_load", d->o_data, 0x11110000);

    // Reset clears
    d->i_rst = 1;
    tick(d);
    d->i_rst = 0;
    check("reset_clears", d->o_data, 0);

    printf("mdr: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}

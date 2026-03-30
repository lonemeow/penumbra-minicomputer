// Verilator testbench for smoke_adder
// Verifies the toolchain works: compile, simulate, check results.

#include <cstdio>
#include <cstdlib>
#include "Vsmoke_adder.h"

int main(int argc, char** argv) {
    Vsmoke_adder* dut = new Vsmoke_adder;

    // Test cases: {a, b, expected_sum, expected_carry}
    struct { uint32_t a, b, sum; int carry; } tests[] = {
        {0,          0,          0,          0},
        {1,          2,          3,          0},
        {0xFFFFFFFF, 1,          0,          1},
        {0x80000000, 0x80000000, 0,          1},
        {0xDEADBEEF, 0x12345678, 0xF0E21567, 0},
    };

    int pass = 0, fail = 0;
    int n = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < n; i++) {
        dut->i_a = tests[i].a;
        dut->i_b = tests[i].b;
        dut->eval();

        bool ok = (dut->o_sum == tests[i].sum) &&
                  (dut->o_carry == tests[i].carry);

        if (ok) {
            pass++;
        } else {
            fail++;
            printf("FAIL test %d: 0x%08X + 0x%08X = 0x%08X (carry %d), "
                   "expected 0x%08X (carry %d)\n",
                   i, tests[i].a, tests[i].b,
                   dut->o_sum, dut->o_carry,
                   tests[i].sum, tests[i].carry);
        }
    }

    printf("smoke_adder: %d/%d tests passed\n", pass, n);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

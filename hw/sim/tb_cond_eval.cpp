// Verilator testbench for the Penumbra condition evaluator
//
// Exhaustively tests all 256 combinations (16 conditions × 16 flag states).
// The reference model is a C function that mirrors the spec.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vcond_eval.h"

// Condition codes (must match penumbra_pkg.sv)
enum Cond {
    COND_AL = 0b0000, COND_EQ = 0b0001, COND_NE = 0b0010, COND_CS = 0b0011,
    COND_CC = 0b0100, COND_MI = 0b0101, COND_PL = 0b0110, COND_VS = 0b0111,
    COND_VC = 0b1000, COND_HI = 0b1001, COND_LS = 0b1010, COND_GE = 0b1011,
    COND_LT = 0b1100, COND_GT = 0b1101, COND_LE = 0b1110, COND_BL = 0b1111,
};

static const char* cond_name[] = {
    "AL", "EQ", "NE", "CS", "CC", "MI", "PL", "VS",
    "VC", "HI", "LS", "GE", "LT", "GT", "LE", "BL",
};

// C reference model — the "golden" implementation to check against
static int ref_cond_eval(int z, int n, int c, int v, int cond) {
    switch (cond) {
        case COND_AL: return 1;
        case COND_EQ: return z;
        case COND_NE: return !z;
        case COND_CS: return c;
        case COND_CC: return !c;
        case COND_MI: return n;
        case COND_PL: return !n;
        case COND_VS: return v;
        case COND_VC: return !v;
        case COND_HI: return c && !z;
        case COND_LS: return !c || z;
        case COND_GE: return n == v;
        case COND_LT: return n != v;
        case COND_GT: return !z && (n == v);
        case COND_LE: return z || (n != v);
        case COND_BL: return 1;
        default:      return 0;
    }
}

int main(int argc, char** argv) {
    Vcond_eval* dut = new Vcond_eval;

    int pass = 0, fail = 0;

    // Exhaustive: 16 flag combinations × 16 condition codes = 256 tests
    for (int flags = 0; flags < 16; flags++) {
        int z = (flags >> 3) & 1;
        int n = (flags >> 2) & 1;
        int c = (flags >> 1) & 1;
        int v = (flags >> 0) & 1;

        dut->i_flag_z = z;
        dut->i_flag_n = n;
        dut->i_flag_c = c;
        dut->i_flag_v = v;

        for (int cond = 0; cond < 16; cond++) {
            dut->i_cond = cond;
            dut->eval();

            int expected = ref_cond_eval(z, n, c, v, cond);
            int got = dut->o_taken;

            if (got == expected) {
                pass++;
            } else {
                fail++;
                printf("  FAIL %s: Z=%d N=%d C=%d V=%d → got %d, expected %d\n",
                       cond_name[cond], z, n, c, v, got, expected);
            }
        }
    }

    printf("cond_eval: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

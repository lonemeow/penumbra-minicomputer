// Verilator testbench for the Penumbra immediate extractor
//
// Tests all three extension modes with positive/negative/edge-case values.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vimm_ext.h"

enum Mode {
    MODE_ZERO_EXT  = 0b00,
    MODE_SIGN_EXT  = 0b01,
    MODE_SHIFT_L16 = 0b10,
};

struct TestCase {
    const char* name;
    uint8_t  mode;
    uint16_t imm16;
    uint32_t expected;
};

int main(int argc, char** argv) {
    Vimm_ext* dut = new Vimm_ext;

    TestCase tests[] = {
        // ── Zero-extend ──────────────────────────────────────────
        {"zext_zero",       MODE_ZERO_EXT,  0x0000, 0x00000000},
        {"zext_small",      MODE_ZERO_EXT,  0x0042, 0x00000042},
        {"zext_max",        MODE_ZERO_EXT,  0xFFFF, 0x0000FFFF},
        {"zext_msb_set",    MODE_ZERO_EXT,  0x8000, 0x00008000},

        // ── Sign-extend ──────────────────────────────────────────
        {"sext_positive",   MODE_SIGN_EXT,  0x0042, 0x00000042},
        {"sext_zero",       MODE_SIGN_EXT,  0x0000, 0x00000000},
        {"sext_max_pos",    MODE_SIGN_EXT,  0x7FFF, 0x00007FFF},
        {"sext_neg_one",    MODE_SIGN_EXT,  0xFFFF, 0xFFFFFFFF},
        {"sext_min_neg",    MODE_SIGN_EXT,  0x8000, 0xFFFF8000},
        {"sext_neg_small",  MODE_SIGN_EXT,  0xFFFE, 0xFFFFFFFE},

        // ── Shift-left-16 ────────────────────────────────────────
        {"sl16_zero",       MODE_SHIFT_L16, 0x0000, 0x00000000},
        {"sl16_one",        MODE_SHIFT_L16, 0x0001, 0x00010000},
        {"sl16_max",        MODE_SHIFT_L16, 0xFFFF, 0xFFFF0000},
        {"sl16_pattern",    MODE_SHIFT_L16, 0xABCD, 0xABCD0000},
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < total; i++) {
        dut->i_imm16 = tests[i].imm16;
        dut->i_mode  = tests[i].mode;
        dut->eval();

        if (dut->o_imm32 == tests[i].expected) {
            pass++;
        } else {
            fail++;
            printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n",
                   tests[i].name, dut->o_imm32, tests[i].expected);
        }
    }

    printf("imm_ext: %d/%d tests passed\n", pass, total);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

// Verilator testbench for the Penumbra Byte Extractor
//
// Tests byte and halfword extraction from a 32-bit word with
// zero-extend and sign-extend, at all valid byte offsets.
// Also verifies word pass-through.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vbyte_ext.h"

struct TestCase {
    const char* name;
    uint32_t data;       // 32-bit memory word
    uint8_t  addr_lo;    // byte offset [1:0]
    uint8_t  size;       // 00=byte, 01=half, 10=word
    uint8_t  sign_ext;   // 0=zero, 1=sign
    uint32_t expected;
};

static const TestCase tests[] = {
    // ── Word pass-through ────────────────────────────────
    {"word pass",       0xDEADBEEF, 0, 0b10, 0, 0xDEADBEEF},
    {"word any offset", 0x12345678, 2, 0b10, 0, 0x12345678},

    // ── Byte zero-extend ─────────────────────────────────
    {"byte0 zext", 0x44332211, 0, 0b00, 0, 0x00000011},
    {"byte1 zext", 0x44332211, 1, 0b00, 0, 0x00000022},
    {"byte2 zext", 0x44332211, 2, 0b00, 0, 0x00000033},
    {"byte3 zext", 0x44332211, 3, 0b00, 0, 0x00000044},
    {"byte0 zext 0xFF", 0x000000FF, 0, 0b00, 0, 0x000000FF},

    // ── Byte sign-extend ─────────────────────────────────
    {"byte0 sext pos",  0x0000007F, 0, 0b00, 1, 0x0000007F},
    {"byte0 sext neg",  0x00000080, 0, 0b00, 1, 0xFFFFFF80},
    {"byte1 sext neg",  0x0000FE00, 1, 0b00, 1, 0xFFFFFFFE},
    {"byte3 sext neg",  0x80000000, 3, 0b00, 1, 0xFFFFFF80},
    {"byte2 sext pos",  0x00010000, 2, 0b00, 1, 0x00000001},

    // ── Halfword zero-extend ─────────────────────────────
    {"half0 zext", 0xBEEF1234, 0, 0b01, 0, 0x00001234},
    {"half2 zext", 0xBEEF1234, 2, 0b01, 0, 0x0000BEEF},
    {"half0 zext 0xFFFF", 0x0000FFFF, 0, 0b01, 0, 0x0000FFFF},

    // ── Halfword sign-extend ─────────────────────────────
    {"half0 sext pos",  0x00007FFF, 0, 0b01, 1, 0x00007FFF},
    {"half0 sext neg",  0x00008000, 0, 0b01, 1, 0xFFFF8000},
    {"half2 sext neg",  0x80001234, 2, 0b01, 1, 0xFFFF8000},
    {"half2 sext pos",  0x7FFF0000, 2, 0b01, 1, 0x00007FFF},
};

int main(int argc, char** argv) {
    Vbyte_ext* dut = new Vbyte_ext;

    int pass = 0, fail = 0;
    int ntests = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < ntests; i++) {
        const TestCase& tc = tests[i];
        dut->i_data     = tc.data;
        dut->i_addr_lo  = tc.addr_lo;
        dut->i_size     = tc.size;
        dut->i_sign_ext = tc.sign_ext;
        dut->eval();

        if (dut->o_data != tc.expected) {
            printf("  FAIL [%s] got 0x%08X, expected 0x%08X\n",
                   tc.name, dut->o_data, tc.expected);
            fail++;
        } else {
            pass++;
        }
    }

    printf("\nbyte_ext: %d/%d tests passed\n", pass, ntests);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

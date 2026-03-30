// Verilator testbench for the Penumbra Byte Replicator
//
// Tests byte and halfword replication across all lanes,
// plus word pass-through.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vbyte_rep.h"

struct TestCase {
    const char* name;
    uint32_t data;
    uint8_t  size;       // 00=byte, 01=half, 10=word
    uint32_t expected;
};

static const TestCase tests[] = {
    // ── Word pass-through ────────────────────────────────
    {"word pass",       0xDEADBEEF, 0b10, 0xDEADBEEF},
    {"word zero",       0x00000000, 0b10, 0x00000000},

    // ── Byte replication ─────────────────────────────────
    {"byte 0x42",       0x00000042, 0b00, 0x42424242},
    {"byte 0xFF",       0x000000FF, 0b00, 0xFFFFFFFF},
    {"byte 0x00",       0x00000000, 0b00, 0x00000000},
    {"byte 0x81",       0xABCD0081, 0b00, 0x81818181},  // upper bits ignored

    // ── Halfword replication ─────────────────────────────
    {"half 0x1234",     0x00001234, 0b01, 0x12341234},
    {"half 0xFFFF",     0x0000FFFF, 0b01, 0xFFFFFFFF},
    {"half 0x8000",     0xDEAD8000, 0b01, 0x80008000},  // upper bits ignored
    {"half 0x0000",     0x00000000, 0b01, 0x00000000},
};

int main(int argc, char** argv) {
    Vbyte_rep* dut = new Vbyte_rep;

    int pass = 0, fail = 0;
    int ntests = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < ntests; i++) {
        const TestCase& tc = tests[i];
        dut->i_data = tc.data;
        dut->i_size = tc.size;
        dut->eval();

        if (dut->o_data != tc.expected) {
            printf("  FAIL [%s] got 0x%08X, expected 0x%08X\n",
                   tc.name, dut->o_data, tc.expected);
            fail++;
        } else {
            pass++;
        }
    }

    printf("\nbyte_rep: %d/%d tests passed\n", pass, ntests);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

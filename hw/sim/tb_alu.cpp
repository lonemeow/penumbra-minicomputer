// Verilator testbench for the Penumbra ALU
//
// Tests all 11 single-cycle operations with edge cases:
//   - Arithmetic: ADD, SUB (including carry/overflow flag corners)
//   - Logic: AND, OR, XOR, NOT
//   - Shifts: SHL, SHR, SAR (including shift-by-0 and sign extension)
//   - Pass-through: PASS_A, PASS_B
//
// Each test checks both the result and all four flags (Z, N, C, V).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Valu.h"

// ALU opcodes (must match alu.sv localparam values)
enum AluOp {
    OP_ADD    = 0b00000,
    OP_SUB    = 0b00001,
    OP_AND    = 0b00010,
    OP_OR     = 0b00011,
    OP_XOR    = 0b00100,
    OP_SHL    = 0b00101,
    OP_SHR    = 0b00110,
    OP_SAR    = 0b00111,
    OP_PASS_A = 0b01000,
    OP_PASS_B = 0b01001,
    OP_NOT    = 0b01010,
};

struct TestCase {
    const char* name;
    uint8_t  op;
    uint32_t a;
    uint32_t b;
    uint32_t expected_result;
    int      expected_z;  // Zero flag
    int      expected_n;  // Negative flag
    int      expected_c;  // Carry flag
    int      expected_v;  // Overflow flag
};

static int run_test(Valu* dut, const TestCase& tc) {
    dut->i_a  = tc.a;
    dut->i_b  = tc.b;
    dut->i_op = tc.op;
    dut->eval();

    int errors = 0;

    if (dut->o_result != tc.expected_result) {
        printf("  FAIL [%s] result: got 0x%08X, expected 0x%08X\n",
               tc.name, dut->o_result, tc.expected_result);
        errors++;
    }
    if (dut->o_flag_z != tc.expected_z) {
        printf("  FAIL [%s] Z flag: got %d, expected %d\n",
               tc.name, dut->o_flag_z, tc.expected_z);
        errors++;
    }
    if (dut->o_flag_n != tc.expected_n) {
        printf("  FAIL [%s] N flag: got %d, expected %d\n",
               tc.name, dut->o_flag_n, tc.expected_n);
        errors++;
    }
    if (dut->o_flag_c != tc.expected_c) {
        printf("  FAIL [%s] C flag: got %d, expected %d\n",
               tc.name, dut->o_flag_c, tc.expected_c);
        errors++;
    }
    if (dut->o_flag_v != tc.expected_v) {
        printf("  FAIL [%s] V flag: got %d, expected %d\n",
               tc.name, dut->o_flag_v, tc.expected_v);
        errors++;
    }

    return errors;
}

int main(int argc, char** argv) {
    Valu* dut = new Valu;

    //                                         name                    op        A            B            result       Z  N  C  V
    TestCase tests[] = {
        // ── ADD ──────────────────────────────────────────────────────────────────────────────────────────────────
        {"add_basic",                          OP_ADD,   0x00000001,  0x00000002, 0x00000003,  0, 0, 0, 0},
        {"add_zero",                           OP_ADD,   0x00000000,  0x00000000, 0x00000000,  1, 0, 0, 0},
        {"add_carry",                          OP_ADD,   0xFFFFFFFF,  0x00000001, 0x00000000,  1, 0, 1, 0},
        {"add_overflow_pos",                   OP_ADD,   0x7FFFFFFF,  0x00000001, 0x80000000,  0, 1, 0, 1},
        {"add_overflow_neg",                   OP_ADD,   0x80000000,  0x80000000, 0x00000000,  1, 0, 1, 1},
        {"add_no_overflow",                    OP_ADD,   0x7FFFFFFF,  0x80000000, 0xFFFFFFFF,  0, 1, 0, 0},

        // ── SUB (ARM-style carry: C=1 means no borrow, A >= B unsigned) ─────────────────────────────────────────
        {"sub_basic",                          OP_SUB,   0x00000005,  0x00000003, 0x00000002,  0, 0, 1, 0},
        {"sub_zero",                           OP_SUB,   0x00000005,  0x00000005, 0x00000000,  1, 0, 1, 0},
        {"sub_borrow",                         OP_SUB,   0x00000003,  0x00000005, 0xFFFFFFFE,  0, 1, 0, 0},
        {"sub_overflow_pos",                   OP_SUB,   0x7FFFFFFF,  0xFFFFFFFF, 0x80000000,  0, 1, 0, 1},
        {"sub_overflow_neg",                   OP_SUB,   0x80000000,  0x00000001, 0x7FFFFFFF,  0, 0, 1, 1},
        {"sub_max_from_zero",                  OP_SUB,   0x00000000,  0xFFFFFFFF, 0x00000001,  0, 0, 0, 0},

        // ── AND ──────────────────────────────────────────────────────────────────────────────────────────────────
        {"and_basic",                          OP_AND,   0xFF00FF00,  0x0F0F0F0F, 0x0F000F00,  0, 0, 0, 0},
        {"and_zero",                           OP_AND,   0xAAAAAAAA,  0x55555555, 0x00000000,  1, 0, 0, 0},
        {"and_negative",                       OP_AND,   0xFFFFFFFF,  0x80000000, 0x80000000,  0, 1, 0, 0},

        // ── OR ───────────────────────────────────────────────────────────────────────────────────────────────────
        {"or_basic",                           OP_OR,    0xFF00FF00,  0x00FF00FF, 0xFFFFFFFF,  0, 1, 0, 0},
        {"or_zero",                            OP_OR,    0x00000000,  0x00000000, 0x00000000,  1, 0, 0, 0},

        // ── XOR ──────────────────────────────────────────────────────────────────────────────────────────────────
        {"xor_basic",                          OP_XOR,   0xFF00FF00,  0x0F0F0F0F, 0xF00FF00F,  0, 1, 0, 0},
        {"xor_same",                           OP_XOR,   0xDEADBEEF,  0xDEADBEEF, 0x00000000,  1, 0, 0, 0},

        // ── SHL ──────────────────────────────────────────────────────────────────────────────────────────────────
        {"shl_by1",                            OP_SHL,   0x00000001,  0x00000001, 0x00000002,  0, 0, 0, 0},
        {"shl_by0",                            OP_SHL,   0x80000001,  0x00000000, 0x80000001,  0, 1, 0, 0},
        {"shl_carry",                          OP_SHL,   0x80000001,  0x00000001, 0x00000002,  0, 0, 1, 0},
        {"shl_by31",                           OP_SHL,   0x00000001,  0x0000001F, 0x80000000,  0, 1, 0, 0},
        // Shift by 32: shamt = B[4:0] = 0, so no shift (32 & 0x1F == 0)
        {"shl_by32_wraps",                     OP_SHL,   0xFFFFFFFF,  0x00000020, 0xFFFFFFFF,  0, 1, 0, 0},

        // ── SHR (logical) ────────────────────────────────────────────────────────────────────────────────────────
        {"shr_by1",                            OP_SHR,   0x80000000,  0x00000001, 0x40000000,  0, 0, 0, 0},
        {"shr_by0",                            OP_SHR,   0x80000001,  0x00000000, 0x80000001,  0, 1, 0, 0},
        {"shr_carry",                          OP_SHR,   0x00000001,  0x00000001, 0x00000000,  1, 0, 1, 0},
        {"shr_no_sign_extend",                 OP_SHR,   0x80000000,  0x0000001F, 0x00000001,  0, 0, 0, 0},

        // ── SAR (arithmetic — preserves sign bit) ────────────────────────────────────────────────────────────────
        {"sar_positive",                       OP_SAR,   0x40000000,  0x00000001, 0x20000000,  0, 0, 0, 0},
        {"sar_negative",                       OP_SAR,   0x80000000,  0x00000001, 0xC0000000,  0, 1, 0, 0},
        {"sar_negative_31",                    OP_SAR,   0x80000000,  0x0000001F, 0xFFFFFFFF,  0, 1, 0, 0},
        {"sar_carry",                          OP_SAR,   0x80000003,  0x00000001, 0xC0000001,  0, 1, 1, 0},

        // ── PASS_A ───────────────────────────────────────────────────────────────────────────────────────────────
        {"pass_a",                             OP_PASS_A,0xDEADBEEF,  0x12345678, 0xDEADBEEF,  0, 1, 0, 0},
        {"pass_a_zero",                        OP_PASS_A,0x00000000,  0xFFFFFFFF, 0x00000000,  1, 0, 0, 0},

        // ── PASS_B ───────────────────────────────────────────────────────────────────────────────────────────────
        {"pass_b",                             OP_PASS_B,0x12345678,  0xDEADBEEF, 0xDEADBEEF,  0, 1, 0, 0},
        {"pass_b_zero",                        OP_PASS_B,0xFFFFFFFF,  0x00000000, 0x00000000,  1, 0, 0, 0},

        // ── NOT (inverts B operand) ──────────────────────────────────────────────────────────────────────────────
        {"not_basic",                          OP_NOT,   0x00000000,  0x00000000, 0xFFFFFFFF,  0, 1, 0, 0},
        {"not_allones",                        OP_NOT,   0x00000000,  0xFFFFFFFF, 0x00000000,  1, 0, 0, 0},
        {"not_pattern",                        OP_NOT,   0x00000000,  0xAAAAAAAA, 0x55555555,  0, 0, 0, 0},
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int pass = 0, fail = 0;

    for (int i = 0; i < total; i++) {
        int errors = run_test(dut, tests[i]);
        if (errors == 0) {
            pass++;
        } else {
            fail++;
        }
    }

    printf("alu: %d/%d tests passed\n", pass, total);
    if (fail > 0) {
        printf("  *** %d FAILED ***\n", fail);
    }

    delete dut;
    return (fail > 0) ? 1 : 0;
}

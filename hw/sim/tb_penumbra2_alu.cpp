// Verilator testbench for penumbra2_alu.
//
// Exercises every alu_op and the NZCV flags, with emphasis on the
// carry and signed-overflow edge cases:
//   - ADD/SUB/ADC/SBC results, carry-out, and ARM carry semantics
//     (C = NOT borrow on subtract).
//   - Signed overflow (V) set only on the add/subtract family, and
//     only when the operand signs agree but the result's sign flips.
//   - Logical ops (AND/OR/XOR/NOT) and PASS clear C and V.
//   - Shifts (SHL/SHR/SAR) and their shift-out carry.
//   - Z and N flags.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_alu.h"

// alu_op encoding (mirror penumbra2_pkg ALU_*).
enum { ADD = 0, SUB = 1, AND = 2, OR = 3, XOR = 4, SHL = 5, SHR = 6,
       SAR = 7, PASS = 8, NOT = 9, ADC = 10, SBC = 11 };

static int errors = 0;
static int tests = 0;

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

static void run(Vpenumbra2_alu* dut, uint32_t a, uint32_t b, int op, int cin = 0) {
    dut->i_a = a;
    dut->i_b = b;
    dut->i_op = op;
    dut->i_carry_in = cin;
    dut->eval();
}

int main() {
    Vpenumbra2_alu* dut = new Vpenumbra2_alu;

    // ── ADD ──────────────────────────────────────────────────────
    run(dut, 2, 3, ADD);
    check("add_result", dut->o_result, 5);
    check("add_no_carry", dut->o_flag_c, 0);
    check("add_no_overflow", dut->o_flag_v, 0);

    run(dut, 0xFFFFFFFF, 1, ADD);
    check("add_wrap_result", dut->o_result, 0);
    check("add_carry_out", dut->o_flag_c, 1);
    check("add_wrap_zero", dut->o_flag_z, 1);

    run(dut, 0x7FFFFFFF, 1, ADD);            // INT_MAX + 1
    check("add_ovf_result", dut->o_result, 0x80000000);
    check("add_ovf_v", dut->o_flag_v, 1);
    check("add_ovf_n", dut->o_flag_n, 1);

    run(dut, 0x80000000, 0x80000000, ADD);   // two negatives
    check("add_neg_ovf_result", dut->o_result, 0);
    check("add_neg_ovf_v", dut->o_flag_v, 1);
    check("add_neg_ovf_c", dut->o_flag_c, 1);

    // ── SUB (ARM carry: C = NOT borrow) ──────────────────────────
    run(dut, 5, 3, SUB);
    check("sub_result", dut->o_result, 2);
    check("sub_no_borrow_c1", dut->o_flag_c, 1);    // no borrow → C=1
    check("sub_no_overflow", dut->o_flag_v, 0);

    run(dut, 3, 5, SUB);
    check("sub_borrow_result", dut->o_result, 0xFFFFFFFE);
    check("sub_borrow_c0", dut->o_flag_c, 0);       // borrow → C=0
    check("sub_borrow_n", dut->o_flag_n, 1);

    run(dut, 0x80000000, 1, SUB);            // INT_MIN - 1
    check("sub_ovf_result", dut->o_result, 0x7FFFFFFF);
    check("sub_ovf_v", dut->o_flag_v, 1);

    run(dut, 0x7FFFFFFF, 0xFFFFFFFF, SUB);   // INT_MAX - (-1)
    check("sub_ovf2_result", dut->o_result, 0x80000000);
    check("sub_ovf2_v", dut->o_flag_v, 1);

    // ── ADC / SBC (carry-in from SR) ─────────────────────────────
    run(dut, 1, 1, ADC, /*cin=*/1);
    check("adc_with_carry", dut->o_result, 3);
    run(dut, 5, 3, SBC, /*cin=*/1);          // A + ~B + 1 = A - B
    check("sbc_carry1", dut->o_result, 2);
    run(dut, 5, 3, SBC, /*cin=*/0);          // A + ~B + 0 = A - B - 1
    check("sbc_carry0", dut->o_result, 1);

    // ── Logical: clear C and V ───────────────────────────────────
    run(dut, 0xF0F0F0F0, 0x0FF00FF0, AND);
    check("and_result", dut->o_result, 0x00F000F0);
    check("and_clears_v", dut->o_flag_v, 0);
    check("and_clears_c", dut->o_flag_c, 0);

    run(dut, 0xF0F0F0F0, 0x0F0F0F0F, OR);
    check("or_result", dut->o_result, 0xFFFFFFFF);
    check("or_n", dut->o_flag_n, 1);

    run(dut, 0xAAAAAAAA, 0xFFFFFFFF, XOR);
    check("xor_result", dut->o_result, 0x55555555);

    run(dut, 0, 0x0F0F0F0F, NOT);            // NOT operates on B
    check("not_result", dut->o_result, 0xF0F0F0F0);
    check("not_clears_v", dut->o_flag_v, 0);

    // ── PASS (MOV via operand B) ─────────────────────────────────
    run(dut, 0xDEADBEEF, 0x12345678, PASS);
    check("pass_result", dut->o_result, 0x12345678);
    check("pass_clears_c", dut->o_flag_c, 0);
    check("pass_clears_v", dut->o_flag_v, 0);

    // ── Shifts and shift-out carry ───────────────────────────────
    run(dut, 0x00000001, 4, SHL);
    check("shl_result", dut->o_result, 0x00000010);
    run(dut, 0x80000000, 1, SHL);
    check("shl_carry_out", dut->o_flag_c, 1);        // MSB shifted out
    run(dut, 0xF0000000, 4, SHR);
    check("shr_result", dut->o_result, 0x0F000000);
    run(dut, 0x80000000, 4, SAR);
    check("sar_sign_fill", dut->o_result, 0xF8000000); // sign-extends
    run(dut, 0x00000008, 1, SHR);
    check("shr_carry_lsb", dut->o_flag_c, 0);          // bit 0 was 0
    run(dut, 0x00000009, 1, SHR);
    check("shr_carry_lsb1", dut->o_flag_c, 1);         // bit 0 was 1

    // ── Z flag ───────────────────────────────────────────────────
    run(dut, 7, 7, SUB);
    check("sub_zero_result", dut->o_result, 0);
    check("sub_zero_z", dut->o_flag_z, 1);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_alu: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

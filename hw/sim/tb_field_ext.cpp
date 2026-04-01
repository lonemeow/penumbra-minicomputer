// Verilator testbench for the Penumbra field extractor
//
// Tests field extraction for all four instruction formats using
// known instruction encodings from the ISA spec.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vfield_ext.h"

static int errors = 0;
static int tests = 0;

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", name, got, expected);
        errors++;
    }
}

int main(int argc, char** argv) {
    Vfield_ext* dut = new Vfield_ext;

    // ── Format R: ADD R3, R7 (op=00000, Rd=3, Rs=7, F=0) ────────
    // Bits: 00 00000 0011 0111 0 0000000000000000
    //       31      25   21  17 16               0
    dut->i_ir = 0b00'00000'0011'0111'0'000000000000000'0;
    dut->eval();

    check("fmtR_format",   dut->o_format,    0b00);
    check("fmtR_op",       dut->o_r_op,      0b00000);
    check("fmtR_rd",       dut->o_r_rd,      3);
    check("fmtR_rs",       dut->o_r_rs,      7);
    check("fmtR_f",        dut->o_r_f,       0);

    // ── Format R: CMP R5, R9 (SUB with F=1, op=00001, Rd=5, Rs=9) ─
    dut->i_ir = 0b00'00001'0101'1001'1'000000000000000'0;
    dut->eval();

    check("fmtR_cmp_op",   dut->o_r_op,      0b00001);
    check("fmtR_cmp_rd",   dut->o_r_rd,      5);
    check("fmtR_cmp_rs",   dut->o_r_rs,      9);
    check("fmtR_cmp_f",    dut->o_r_f,       1);

    // ── Format R: MTSYS R2, dev=5, reg=3 (op=10000) ─────────────
    // spare[15:12]=dev=5, spare[11:8]=reg=3
    dut->i_ir = 0b00'10000'0010'0000'0'0101'0011'00000000;
    dut->eval();

    check("fmtR_mtsys_op",     dut->o_r_op,      0b10000);
    check("fmtR_mtsys_rd",     dut->o_r_rd,      2);
    check("fmtR_mtsys_dev",    dut->o_r_sys_dev, 5);
    check("fmtR_mtsys_reg",    dut->o_r_sys_reg, 3);

    // ── Format L: LLI R10, #0xABCD (op=0000, Rd=10) ─────────────
    // Bits: 01 0000 1010 000000 1010101111001101
    dut->i_ir = 0b01'0000'1010'000000'1010101111001101;
    dut->eval();

    check("fmtL_format",   dut->o_format,    0b01);
    check("fmtL_op",       dut->o_l_op,      0b0000);
    check("fmtL_rd",       dut->o_l_rd,      10);
    check("fmtL_imm16",    dut->o_imm16,     0xABCD);

    // ── Format M: LDW R4, [R8 + 100] (L=1, sz=10, SE=0) ────────
    // offset16 in bits [17:2], value = 100
    uint32_t m_ir = (0b10u << 30) | (1u << 29) | (0b10u << 27) | (0u << 26)
                  | (4u << 22) | (8u << 18) | (100u << 2);
    dut->i_ir = m_ir;
    dut->eval();

    check("fmtM_format",   dut->o_format,      0b10);
    check("fmtM_load",     dut->o_m_load,      1);
    check("fmtM_size",     dut->o_m_size,      0b10);
    check("fmtM_sign_ext", dut->o_m_sign_ext,  0);
    check("fmtM_rd",       dut->o_m_rd,        4);
    check("fmtM_rb",       dut->o_m_rb,        8);
    check("fmtM_offset16", dut->o_m_offset16,  100);

    // ── Format M: STB R1, [R2 + -4] (L=0, sz=00, SE=x) ─────────
    // offset16 = -4 as 16-bit signed = 0xFFFC
    uint32_t m_ir2 = (0b10u << 30) | (0u << 29) | (0b00u << 27) | (0u << 26)
                   | (1u << 22) | (2u << 18) | ((0xFFFCu & 0xFFFF) << 2);
    dut->i_ir = m_ir2;
    dut->eval();

    check("fmtM_stb_load",     dut->o_m_load,     0);
    check("fmtM_stb_size",     dut->o_m_size,     0b00);
    check("fmtM_stb_rd",       dut->o_m_rd,       1);
    check("fmtM_stb_rb",       dut->o_m_rb,       2);
    check("fmtM_stb_offset16", dut->o_m_offset16, 0xFFFC);

    // ── Format B: BEQ +256 (cond=0001, offset22=256) ────────────
    uint32_t b_ir = (0b11u << 30) | (0b0001u << 26) | (256u << 4);
    dut->i_ir = b_ir;
    dut->eval();

    check("fmtB_format",    dut->o_format,      0b11);
    check("fmtB_cond",      dut->o_b_cond,      0b0001);
    check("fmtB_offset22",  dut->o_b_offset22,  256);

    // ── Format B: BL -1 (cond=1111, offset22=0x3FFFFF = -1 signed) ─
    uint32_t bl_ir = (0b11u << 30) | (0b1111u << 26) | (0x3FFFFFu << 4);
    dut->i_ir = bl_ir;
    dut->eval();

    check("fmtB_bl_cond",      dut->o_b_cond,     0b1111);
    check("fmtB_bl_offset22",  dut->o_b_offset22, 0x3FFFFF);

    // ── Summary ──────────────────────────────────────────────────
    printf("field_ext: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

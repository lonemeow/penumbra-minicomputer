// Verilator testbench for penumbra2_decode.
//
// Drives instruction words across all four formats and checks the
// decoded control bundle against doc/internals/penumbra2/control-decode.md
// and the encoding in doc/system/instruction-encoding.md:
//   - op_class for every opcode group.
//   - the architectural register references + is_spr + enables that
//     feed penumbra2_regmap (incl. the store's two reads, divmul's Rdh,
//     RDSPR/WRSPR's SPR source/dest).
//   - alu_op (sliced for Format R, remapped for Format L).
//   - flag interaction (writes_flags / reads_flags / flag_only).
//   - mem controls, branch condition, drain-commit, privilege fault.
//   - immediate extension (the per-format / per-opcode rules).
//
// The immediate-extension group exercises the Format-L immediate logic.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_decode.h"

// op_class (penumbra2_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_BRANCH = 3,
       OPC_JMP = 4, OPC_DIVMUL = 5, OPC_RDSPR = 6, OPC_WRSPR = 7,
       OPC_RDSYS = 8, OPC_WRSYS = 9, OPC_ERET = 10, OPC_EI = 11,
       OPC_DI = 12, OPC_SYSCALL = 13, OPC_BREAK = 14, OPC_ILLEGAL = 15 };
// alu_op (penumbra2_pkg)
enum { ALU_ADD = 0, ALU_SUB = 1, ALU_AND = 2, ALU_OR = 3, ALU_XOR = 4,
       ALU_SHL = 5, ALU_SHR = 6, ALU_SAR = 7, ALU_PASS = 8, ALU_NOT = 9,
       ALU_ADC = 10, ALU_SBC = 11 };
// mem_op (penumbra2_pkg)
enum { MEM_NONE = 0, MEM_LOAD = 1, MEM_STORE = 2 };
// Format R opcodes (5-bit)
enum { OP_R_ADD = 0, OP_R_SUB = 1, OP_R_MOV = 8, OP_R_NOT = 9,
       OP_R_ADC = 10, OP_R_SBC = 11,
       OP_R_MUL = 16, OP_R_WRSYS = 23, OP_R_RDSYS = 24, OP_R_SYSCALL = 25,
       OP_R_BREAK = 26, OP_R_ERET = 27, OP_R_EI = 28, OP_R_DI = 29,
       OP_R_WRSPR = 30, OP_R_RDSPR = 31 };
// Format L opcodes (4-bit)
enum { OP_L_LLI = 0, OP_L_LLIS = 1, OP_L_LUI = 2, OP_L_ADDI = 3,
       OP_L_SUBI = 4, OP_L_CMPI = 5, OP_L_ANDI = 6, OP_L_TESTI = 7,
       OP_L_SHLI = 8, OP_L_SHRI = 9, OP_L_SARI = 10, OP_L_JMP = 11,
       OP_L_JALR = 12 };
// SPR numbers (penumbra_pkg)
enum { SPR_ESR = 0, SPR_EPC = 1, SPR_USP = 2, SPR_SR = 3, SPR_SCR0 = 4 };
// Vector numbers (penumbra_pkg)
enum { VEC_PRIV = 4, VEC_SYSCALL = 5, VEC_BREAK = 6, VEC_ILLEGAL = 7 };
enum { REG_LR = 13 };

static int errors = 0;
static int tests = 0;

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%X (%u), expected 0x%X (%u)\n",
               name, got, got, expected, expected);
        errors++;
    }
}

// ── Instruction word encoders (match instruction-encoding.md) ────
static uint32_t enc_r(int op, int rd, int rs, int f,
                      int field1512 = 0, int field118 = 0) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16)
         | ((field1512 & 0xF) << 12) | ((field118 & 0xF) << 8);
}
static uint32_t enc_l(int op, int rd, uint16_t imm) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22) | imm;
}
static uint32_t enc_m(int L, int sz, int se, int rd, int rb, uint16_t off) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22) | ((rb & 0xF) << 18) | ((uint32_t)off << 2);
}
static uint32_t enc_b(int cond, uint32_t off22) {
    return (3u << 30) | ((cond & 0xF) << 26) | ((off22 & 0x3FFFFF) << 4);
}

static void decode(Vpenumbra2_decode* dut, uint32_t ir, int sup) {
    dut->i_ir = ir;
    dut->i_supervisor = sup;
    dut->eval();
}

int main() {
    Vpenumbra2_decode* dut = new Vpenumbra2_decode;

    // ── Format R: ADD R1, R2 (Rd=R1 dest+srcA, Rs=R2 srcB) ───────
    decode(dut, enc_r(OP_R_ADD, 1, 2, 0), 0);
    check("add_class",    dut->o_op_class, OPC_ALU);
    check("add_aluop",    dut->o_alu_op, ALU_ADD);
    check("add_srca",     dut->o_src_a_sel, 1);
    check("add_srca_en",  dut->o_src_a_en, 1);
    check("add_srcb",     dut->o_src_b_sel, 2);
    check("add_srcb_en",  dut->o_src_b_en, 1);
    check("add_dst",      dut->o_dst_sel, 1);
    check("add_dst_en",   dut->o_dst_en, 1);
    check("add_gpr_we",   dut->o_gpr_we, 1);
    check("add_wflags",   dut->o_writes_flags, 1);
    check("add_flag_we",  dut->o_flag_we, 1);
    check("add_flagonly", dut->o_flag_only, 0);
    check("add_b_imm",    dut->o_b_from_imm, 0);
    check("add_a_pc",     dut->o_a_from_pc, 0);
    check("add_memop",    dut->o_mem_op, MEM_NONE);
    check("add_illegal",  dut->o_illegal, 0);

    // CMP = SUB with F=1: flags kept, GPR write dropped.
    decode(dut, enc_r(OP_R_SUB, 3, 4, 1), 0);
    check("cmp_aluop",    dut->o_alu_op, ALU_SUB);
    check("cmp_flagonly", dut->o_flag_only, 1);
    check("cmp_gpr_we",   dut->o_gpr_we, 0);
    check("cmp_wflags",   dut->o_writes_flags, 1);

    // MOV R5, R6: passes Rs; reads no Rd; no flags.
    decode(dut, enc_r(OP_R_MOV, 5, 6, 0), 0);
    check("mov_aluop",    dut->o_alu_op, ALU_PASS);
    check("mov_srca_en",  dut->o_src_a_en, 0);
    check("mov_srcb",     dut->o_src_b_sel, 6);
    check("mov_srcb_en",  dut->o_src_b_en, 1);
    check("mov_dst",      dut->o_dst_sel, 5);
    check("mov_wflags",   dut->o_writes_flags, 0);
    check("mov_gpr_we",   dut->o_gpr_we, 1);

    // NOT R5, R6: ~Rs; reads no Rd; writes flags.
    decode(dut, enc_r(OP_R_NOT, 5, 6, 0), 0);
    check("not_aluop",    dut->o_alu_op, ALU_NOT);
    check("not_srca_en",  dut->o_src_a_en, 0);
    check("not_srcb_en",  dut->o_src_b_en, 1);
    check("not_wflags",   dut->o_writes_flags, 1);

    // ADC R3, R4: Rd + Rs + carry; reads Rd, Rs, and NZCV (carry, via
    // the flag bypass — reads_flags, not a scoreboard source).
    decode(dut, enc_r(OP_R_ADC, 3, 4, 0), 0);
    check("adc_aluop",    dut->o_alu_op, ALU_ADC);
    check("adc_srca",     dut->o_src_a_sel, 3);
    check("adc_srca_en",  dut->o_src_a_en, 1);
    check("adc_srcb_en",  dut->o_src_b_en, 1);
    check("adc_wflags",   dut->o_writes_flags, 1);
    check("adc_rflags",   dut->o_reads_flags, 1);
    decode(dut, enc_r(OP_R_SBC, 3, 4, 0), 0);
    check("sbc_aluop",    dut->o_alu_op, ALU_SBC);
    check("sbc_rflags",   dut->o_reads_flags, 1);
    // A plain ADD does not read flags.
    decode(dut, enc_r(OP_R_ADD, 1, 2, 0), 0);
    check("add_no_rflags", dut->o_reads_flags, 0);

    // Reserved Format R opcode (01100) → illegal.
    decode(dut, enc_r(12, 1, 2, 0), 0);
    check("rsvd_class",   dut->o_op_class, OPC_ILLEGAL);
    check("rsvd_illegal", dut->o_illegal, 1);
    check("rsvd_vec",     dut->o_fault_vec, VEC_ILLEGAL);
    check("rsvd_gpr_we",  dut->o_gpr_we, 0);

    // MUL R1, R2, Rdh=R3.
    decode(dut, enc_r(OP_R_MUL, 1, 2, 0, /*field1512=Rdh*/3), 0);
    check("mul_class",     dut->o_op_class, OPC_DIVMUL);
    check("mul_srca",      dut->o_src_a_sel, 1);
    check("mul_srcb",      dut->o_src_b_sel, 2);
    check("mul_dst",       dut->o_dst_sel, 1);
    check("mul_dsthi",     dut->o_dst_aux_sel, 3);
    check("mul_dsthi_en",  dut->o_dst_aux_en, 1);
    check("mul_gpr_we",    dut->o_gpr_we, 1);
    check("mul_wflags",    dut->o_writes_flags, 1);

    // WRSPR ESR, R4 (supervisor): SPR dest, GPR source, not drain. The value
    // register is the Rd field (as the assembler encodes it), read via src B.
    decode(dut, enc_r(OP_R_WRSPR, 4, 0, 0, SPR_ESR), 1);
    check("wrspr_class",   dut->o_op_class, OPC_WRSPR);
    check("wrspr_srcb",    dut->o_src_b_sel, 4);
    check("wrspr_srcb_en", dut->o_src_b_en, 1);
    check("wrspr_dst",     dut->o_dst_sel, SPR_ESR);
    check("wrspr_dst_spr", dut->o_dst_is_spr, 1);
    check("wrspr_dst_en",  dut->o_dst_en, 1);
    check("wrspr_spr_we",  dut->o_spr_we, 1);
    check("wrspr_sprsel",  dut->o_spr_sel, SPR_ESR);
    check("wrspr_priv",    dut->o_priv_fault, 0);
    check("wrspr_drain",   dut->o_drain_commit, 0);
    // Same op in user mode → privilege fault.
    decode(dut, enc_r(OP_R_WRSPR, 4, 0, 0, SPR_ESR), 0);
    check("wrspr_u_priv",  dut->o_priv_fault, 1);
    check("wrspr_u_vec",   dut->o_fault_vec, VEC_PRIV);
    // WRSPR SR → reserved (illegal): SR is not WRSPR-writable (S/I change via
    // exception entry / ERET, NZCV via flag ops), so it decodes to
    // OPC_ILLEGAL with no destination, value source, or SPR-file write.
    decode(dut, enc_r(OP_R_WRSPR, 4, 0, 0, SPR_SR), 1);
    check("wrspr_sr_illegal", dut->o_op_class, OPC_ILLEGAL);
    check("wrspr_sr_drain",   dut->o_drain_commit, 0);
    check("wrspr_sr_dst_en",  dut->o_dst_en, 0);
    check("wrspr_sr_spr_we",  dut->o_spr_we, 0);

    // RDSPR R5, SCR0 (supervisor): GPR dest; the SPR value reads as operand B
    // (so ALU_PASS, which passes B, carries it to the result).
    decode(dut, enc_r(OP_R_RDSPR, 5, 0, 0, SPR_SCR0), 1);
    check("rdspr_class",   dut->o_op_class, OPC_RDSPR);
    check("rdspr_dst",     dut->o_dst_sel, 5);
    check("rdspr_dst_en",  dut->o_dst_en, 1);
    check("rdspr_srcb",    dut->o_src_b_sel, SPR_SCR0);
    check("rdspr_srcb_spr", dut->o_src_b_is_spr, 1);
    check("rdspr_srcb_en", dut->o_src_b_en, 1);
    check("rdspr_gpr_we",  dut->o_gpr_we, 1);
    // RDSPR SR reads NZCV via the bypass + S/I from committed SR (composed in
    // EX); SR is not a scoreboard source, so neither operand reads it.
    decode(dut, enc_r(OP_R_RDSPR, 5, 0, 0, SPR_SR), 1);
    check("rdspr_sr_rflag",   dut->o_reads_flags, 1);
    check("rdspr_sr_srca_en", dut->o_src_a_en, 0);
    check("rdspr_sr_srcb_en", dut->o_src_b_en, 0);
    check("rdspr_sr_dst_en",  dut->o_dst_en, 1);

    // SYSCALL / BREAK: traps, not privileged.
    decode(dut, enc_r(OP_R_SYSCALL, 0, 0, 0), 0);
    check("sys_class",  dut->o_op_class, OPC_SYSCALL);
    check("sys_trap",   dut->o_is_trap, 1);
    check("sys_vec",    dut->o_fault_vec, VEC_SYSCALL);
    check("sys_priv",   dut->o_priv_fault, 0);
    decode(dut, enc_r(OP_R_BREAK, 0, 0, 0), 0);
    check("brk_class",  dut->o_op_class, OPC_BREAK);
    check("brk_vec",    dut->o_fault_vec, VEC_BREAK);

    // ERET / EI / DI: drain-commit, privileged.
    decode(dut, enc_r(OP_R_ERET, 0, 0, 0), 1);
    check("eret_class", dut->o_op_class, OPC_ERET);
    check("eret_drain", dut->o_drain_commit, 1);
    check("eret_wflag", dut->o_writes_flags, 1);
    decode(dut, enc_r(OP_R_EI, 0, 0, 0), 1);
    check("ei_drain",   dut->o_drain_commit, 1);
    decode(dut, enc_r(OP_R_DI, 0, 0, 0), 0);
    check("di_priv",    dut->o_priv_fault, 1);

    // WRSYS / RDSYS: privileged; WRSYS drains + post-commit waits. WRSYS's
    // value register is the Rd field (as the assembler encodes it); RDSYS's
    // destination is likewise the Rd field.
    decode(dut, enc_r(OP_R_WRSYS, 7, 0, 0), 1);
    check("wrsys_class",  dut->o_op_class, OPC_WRSYS);
    check("wrsys_srcb",   dut->o_src_b_sel, 7);
    check("wrsys_drain",  dut->o_drain_commit, 1);
    check("wrsys_pcwait", dut->o_post_commit_wait, 1);
    decode(dut, enc_r(OP_R_RDSYS, 5, 0, 0), 1);
    check("rdsys_class",  dut->o_op_class, OPC_RDSYS);
    check("rdsys_dst",    dut->o_dst_sel, 5);
    check("rdsys_gpr_we", dut->o_gpr_we, 1);

    // ── Format L ─────────────────────────────────────────────────
    decode(dut, enc_l(OP_L_LLI, 7, 0x1234), 0);
    check("lli_class",   dut->o_op_class, OPC_ALU);
    check("lli_aluop",   dut->o_alu_op, ALU_PASS);
    check("lli_b_imm",   dut->o_b_from_imm, 1);
    check("lli_dst",     dut->o_dst_sel, 7);
    check("lli_srca_en", dut->o_src_a_en, 0);
    check("lli_wflags",  dut->o_writes_flags, 0);

    decode(dut, enc_l(OP_L_ADDI, 3, 0x10), 0);
    check("addi_aluop",  dut->o_alu_op, ALU_ADD);
    check("addi_b_imm",  dut->o_b_from_imm, 1);
    check("addi_srca",   dut->o_src_a_sel, 3);
    check("addi_dst",    dut->o_dst_sel, 3);
    check("addi_wflags", dut->o_writes_flags, 1);

    decode(dut, enc_l(OP_L_CMPI, 3, 0x10), 0);
    check("cmpi_aluop",  dut->o_alu_op, ALU_SUB);
    check("cmpi_flgonly", dut->o_flag_only, 1);
    check("cmpi_gpr_we", dut->o_gpr_we, 0);

    decode(dut, enc_l(OP_L_LUI, 4, 0xABCD), 0);
    check("lui_aluop",   dut->o_alu_op, ALU_OR);
    check("lui_srca",    dut->o_src_a_sel, 4);
    check("lui_srca_en", dut->o_src_a_en, 1);
    check("lui_wflags",  dut->o_writes_flags, 0);

    decode(dut, enc_l(OP_L_JMP, 14, 0), 0);
    check("jmp_class",   dut->o_op_class, OPC_JMP);
    check("jmp_srca",    dut->o_src_a_sel, 14);
    check("jmp_gpr_we",  dut->o_gpr_we, 0);

    decode(dut, enc_l(OP_L_JALR, 14, 0), 0);
    check("jalr_class",  dut->o_op_class, OPC_JMP);
    check("jalr_srca",   dut->o_src_a_sel, 14);
    check("jalr_dst",    dut->o_dst_sel, REG_LR);
    check("jalr_gpr_we", dut->o_gpr_we, 1);

    decode(dut, enc_l(13, 0, 0), 0);   // reserved Format L
    check("l_rsvd",      dut->o_op_class, OPC_ILLEGAL);

    // ── Format M ─────────────────────────────────────────────────
    decode(dut, enc_m(/*L*/1, /*sz=word*/2, /*se*/0, /*rd*/5, /*rb*/6, 0), 0);
    check("ldw_class",   dut->o_op_class, OPC_LOAD);
    check("ldw_memop",   dut->o_mem_op, MEM_LOAD);
    check("ldw_size",    dut->o_mem_size, 2);
    check("ldw_se",      dut->o_sign_ext, 0);
    check("ldw_base",    dut->o_src_a_sel, 6);
    check("ldw_base_en", dut->o_src_a_en, 1);
    check("ldw_dst",     dut->o_dst_sel, 5);
    check("ldw_gpr_we",  dut->o_gpr_we, 1);
    check("ldw_b_imm",   dut->o_b_from_imm, 1);
    check("ldw_aluop",   dut->o_alu_op, ALU_ADD);
    check("ldw_srcb_en", dut->o_src_b_en, 0);

    decode(dut, enc_m(1, /*byte*/0, /*se*/1, 5, 6, 0), 0);
    check("ldbs_size",   dut->o_mem_size, 0);
    check("ldbs_se",     dut->o_sign_ext, 1);

    decode(dut, enc_m(/*store*/0, 2, 0, /*rd=data*/5, /*rb=base*/6, 0), 0);
    check("stw_class",   dut->o_op_class, OPC_STORE);
    check("stw_memop",   dut->o_mem_op, MEM_STORE);
    check("stw_base",    dut->o_src_a_sel, 6);
    check("stw_base_en", dut->o_src_a_en, 1);
    check("stw_data",    dut->o_src_b_sel, 5);
    check("stw_data_en", dut->o_src_b_en, 1);
    check("stw_gpr_we",  dut->o_gpr_we, 0);
    check("stw_b_imm",   dut->o_b_from_imm, 1);

    // ── Format B ─────────────────────────────────────────────────
    decode(dut, enc_b(/*BEQ*/1, 0), 0);
    check("beq_class",   dut->o_op_class, OPC_BRANCH);
    check("beq_cond",    dut->o_cond, 1);
    check("beq_rflags",  dut->o_reads_flags, 1);
    check("beq_a_pc",    dut->o_a_from_pc, 1);
    check("beq_b_imm",   dut->o_b_from_imm, 1);
    check("beq_aluop",   dut->o_alu_op, ALU_ADD);
    check("beq_gpr_we",  dut->o_gpr_we, 0);

    decode(dut, enc_b(/*AL*/0, 0), 0);
    check("bal_rflags",  dut->o_reads_flags, 0);

    decode(dut, enc_b(/*BL*/15, 0), 0);
    check("bl_rflags",   dut->o_reads_flags, 0);
    check("bl_dst",      dut->o_dst_sel, REG_LR);
    check("bl_gpr_we",   dut->o_gpr_we, 1);

    // ── Immediate extension (exercises Format-L imm logic) ───────
    decode(dut, enc_l(OP_L_LLI, 7, 0x1234), 0);
    check("imm_lli_zext",  dut->o_imm, 0x00001234);
    decode(dut, enc_l(OP_L_LLIS, 7, 0x8001), 0);
    check("imm_llis_sext", dut->o_imm, 0xFFFF8001);
    decode(dut, enc_l(OP_L_ADDI, 3, 0xFFFF), 0);
    check("imm_addi_zext", dut->o_imm, 0x0000FFFF);
    decode(dut, enc_l(OP_L_LUI, 4, 0xABCD), 0);
    check("imm_lui_shift", dut->o_imm, 0xABCD0000);
    decode(dut, enc_l(OP_L_SHLI, 3, 0x00E5), 0);   // shift amount = imm[4:0] = 5
    check("imm_shli_amt",  dut->o_imm, 0x00000005);
    // Format M offset: signed byte offset (-1).
    decode(dut, enc_m(1, 2, 0, 5, 6, 0xFFFF), 0);
    check("imm_m_sext",    dut->o_imm, 0xFFFFFFFF);
    // Format B offset: sign-extended word offset << 2. off22 = 0x3FFFFF
    // (-1) → (-1 << 2) = 0xFFFFFFFC.
    decode(dut, enc_b(0, 0x3FFFFF), 0);
    check("imm_b_sext",    dut->o_imm, 0xFFFFFFFC);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_decode: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

// Verilator testbench for penumbra2_ex_stage (straight-line core).
//
// Drives the ID/EX input, the flag-bypass external sources, and the
// pipeline handshake across clock edges, checking the EX/MEM register
// and the back-pressure output against the EX-stage contract in
// doc/internals/penumbra2/{pipeline-stages,control-decode}.md:
//   - reset clears valid
//   - an ALU op latches result + NZCV + the narrowed ctrl into EX/MEM
//   - the control bundle + store data + PC + fault tag pass through
//   - ADC takes its carry-in from the forwarded NZCV: WB producer, the
//     committed SR fallback, and the EX/MEM self-feedback (MEM) leg
//   - a downstream stall holds EX/MEM; a squash bubbles it
//
// NZCV bundle packs as SR[3:0]: N=bit0 Z=bit1 C=bit2 V=bit3.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_ex_stage.h"

// op_class / alu_op / mem_op (penumbra2_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_BRANCH = 3, OPC_JMP = 4 };
enum { ALU_ADD = 0, ALU_SUB = 1, ALU_ADC = 10 };
enum { MEM_NONE = 0, MEM_STORE = 2 };
// Branch condition codes (penumbra_pkg COND_*)
enum { COND_AL = 0, COND_EQ = 1, COND_BL = 15 };
// physical scoreboard entry for R13 (the link register)
enum { PHYS_LR = 13 };
// NZCV bundle bit positions
enum { FN = 0, FZ = 1, FC = 2, FV = 3 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_ex_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Reset the per-cycle inputs to a quiet baseline (valid op_class, no
// memory op, no flags in flight, no stall/squash).
static void clear(Vpenumbra2_ex_stage* dut) {
    dut->i_op_class = OPC_ALU; dut->i_alu_op = ALU_ADD;
    dut->i_op_a = 0; dut->i_op_b = 0; dut->i_store_data = 0;
    dut->i_cond = COND_AL;
    dut->i_mem_op = MEM_NONE; dut->i_mem_size = 0; dut->i_sign_ext = 0;
    dut->i_sys_dev = 0; dut->i_sys_reg = 0; dut->i_spr_sel = 0;
    dut->i_gpr_we = 0; dut->i_spr_we = 0; dut->i_flag_we = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_hi = 0; dut->i_phys_dst_hi_en = 0;
    dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
    dut->i_fault_pending = 0; dut->i_fault_vec = 0;
    dut->i_sr_flags = 0; dut->i_wb_flags = 0; dut->i_wb_writes_flags = 0;
    dut->i_stall_in = 0; dut->i_bubble = 0;
}

// Drive a bubble through EX/MEM so the next test sees no in-flight flag
// producer on the self-feedback (MEM) leg — conditional-branch tests can
// then control the forwarded NZCV through i_sr_flags alone.
static void flush_bubble(Vpenumbra2_ex_stage* dut) {
    clear(dut); dut->i_valid = 0;
    dut->eval(); tick(dut); dut->eval();
}

int main() {
    Vpenumbra2_ex_stage* dut = new Vpenumbra2_ex_stage;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_valid_low", dut->o_valid, 0);

    // ── ALU op: ADD 0 + 0 → result 0, Z set ──────────────────────
    clear(dut);
    dut->i_op_a = 0; dut->i_op_b = 0;
    dut->i_alu_op = ALU_ADD; dut->i_gpr_we = 1; dut->i_flag_we = 1;
    dut->i_phys_dst = 1; dut->i_pc = 0xFFFF0040; dut->i_valid = 1;
    dut->eval();
    check("add_no_stall", dut->o_stall, 0);
    tick(dut); dut->eval();
    check("add_valid",     dut->o_valid, 1);
    check("add_result",    dut->o_result, 0);
    check("add_flag_z",    (dut->o_flag_value >> FZ) & 1, 1);
    check("add_flag_n",    (dut->o_flag_value >> FN) & 1, 0);
    check("add_flag_c",    (dut->o_flag_value >> FC) & 1, 0);
    check("add_op_class",  dut->o_op_class, OPC_ALU);
    check("add_gpr_we",    dut->o_gpr_we, 1);
    check("add_flag_we",   dut->o_flag_we, 1);
    check("add_phys_dst",  dut->o_phys_dst, 1);
    check("add_pc",        dut->o_pc, 0xFFFF0040);
    check("add_fault",     dut->o_fault_pending, 0);

    // ── ALU op: SUB 5 - 3 → result 2, C set (no borrow) ──────────
    clear(dut);
    dut->i_op_a = 5; dut->i_op_b = 3; dut->i_alu_op = ALU_SUB;
    dut->i_gpr_we = 1; dut->i_flag_we = 1; dut->i_phys_dst = 2; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("sub_result",  dut->o_result, 2);
    check("sub_flag_c",  (dut->o_flag_value >> FC) & 1, 1);
    check("sub_flag_z",  (dut->o_flag_value >> FZ) & 1, 0);

    // ── ctrl_mem + store_data pass-through (STORE) ───────────────
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = 2;
    dut->i_op_a = 0x6BA5E000;        // base+offset (ALU computes the EA)
    dut->i_op_b = 0;
    dut->i_store_data = 0x57047A00;  // value to store
    dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("store_op_class",   dut->o_op_class, OPC_STORE);
    check("store_mem_op",     dut->o_mem_op, MEM_STORE);
    check("store_mem_size",   dut->o_mem_size, 2);
    check("store_result_ea",  dut->o_result, 0x6BA5E000);
    check("store_data_pass",  dut->o_store_data, 0x57047A00);

    // ── ADC carry-in from the WB producer ────────────────────────
    // No EX/MEM flag producer (prior slot wrote no flags); WB provides
    // C=1 → ADC adds it: 0x10 + 0x20 + 1 = 0x31.
    clear(dut);
    dut->i_op_a = 0x10; dut->i_op_b = 0x20; dut->i_alu_op = ALU_ADC;
    dut->i_gpr_we = 1; dut->i_phys_dst = 4; dut->i_valid = 1;
    dut->i_wb_writes_flags = 1; dut->i_wb_flags = (1 << FC);  // C=1
    dut->eval(); tick(dut); dut->eval();
    check("adc_wb_carry", dut->o_result, 0x31);

    // ── ADC carry-in from committed SR (no MEM/WB producer) ──────
    clear(dut);
    dut->i_op_a = 0x10; dut->i_op_b = 0x20; dut->i_alu_op = ALU_ADC;
    dut->i_gpr_we = 1; dut->i_phys_dst = 4; dut->i_valid = 1;
    dut->i_sr_flags = (1 << FC);   // committed C=1, nothing in flight
    dut->eval(); tick(dut); dut->eval();
    check("adc_sr_carry", dut->o_result, 0x31);

    // ── ADC carry-in from the EX/MEM self-feedback (MEM leg) ─────
    // Cycle A: ADD 0xFFFFFFFF + 1 → result 0, C=1, writes flags. It
    // now sits in EX/MEM as the youngest producer. Cycle B: ADC reads
    // that carry via the internal MEM feedback, beating SR.
    clear(dut);
    dut->i_op_a = 0xFFFFFFFF; dut->i_op_b = 1; dut->i_alu_op = ALU_ADD;
    dut->i_gpr_we = 1; dut->i_flag_we = 1; dut->i_phys_dst = 6; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("selffeed_add_result", dut->o_result, 0);
    check("selffeed_add_carry",  (dut->o_flag_value >> FC) & 1, 1);
    check("selffeed_add_flagwe", dut->o_flag_we, 1);
    // Cycle B: ADC, with SR carry deliberately 0 so only the MEM
    // feedback can supply the carry.
    dut->i_op_a = 0x10; dut->i_op_b = 0x20; dut->i_alu_op = ALU_ADC;
    dut->i_gpr_we = 1; dut->i_flag_we = 0; dut->i_phys_dst = 4; dut->i_valid = 1;
    dut->i_sr_flags = 0; dut->i_wb_writes_flags = 0;
    dut->eval(); tick(dut); dut->eval();
    check("selffeed_adc_carry", dut->o_result, 0x31);

    // ── Downstream stall holds EX/MEM ────────────────────────────
    clear(dut);
    dut->i_op_a = 7; dut->i_op_b = 8; dut->i_alu_op = ALU_ADD;
    dut->i_gpr_we = 1; dut->i_phys_dst = 9; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("held_issued", dut->o_valid, 1);
    uint32_t held_result = dut->o_result;
    dut->i_stall_in = 1;
    dut->i_op_a = 100; dut->i_op_b = 200;   // a different insn knocking
    dut->i_phys_dst = 10;
    dut->eval();
    check("stall_in_backpressure", dut->o_stall, 1);
    tick(dut); dut->eval();
    check("stall_in_holds_valid",  dut->o_valid, 1);
    check("stall_in_holds_result", dut->o_result, held_result);
    check("stall_in_holds_dst",    dut->o_phys_dst, 9);
    // Hold a second cycle: a multi-cycle stall (e.g. a D-cache miss in
    // MEM) must not let the held slot drift.
    tick(dut); dut->eval();
    check("stall_in_holds_valid2",  dut->o_valid, 1);
    check("stall_in_holds_result2", dut->o_result, held_result);
    check("stall_in_holds_dst2",    dut->o_phys_dst, 9);
    // Release: the instruction that was knocking finally advances — it
    // was back-pressured, not dropped.
    dut->i_stall_in = 0;
    dut->eval(); tick(dut); dut->eval();
    check("stall_release_advances_dst",    dut->o_phys_dst, 10);
    check("stall_release_advances_result", dut->o_result, 300);  // 100 + 200

    // ── i_bubble squashes the in-flight slot ─────────────────────
    clear(dut);
    dut->i_op_a = 1; dut->i_op_b = 2; dut->i_gpr_we = 1; dut->i_valid = 1;
    dut->i_bubble = 1;
    dut->eval(); tick(dut); dut->eval();
    check("bubble_forces_bubble", dut->o_valid, 0);

    // ── Fault tag passes through ─────────────────────────────────
    clear(dut);
    dut->i_op_a = 1; dut->i_op_b = 2; dut->i_valid = 1;
    dut->i_fault_pending = 1; dut->i_fault_vec = 3;
    dut->eval(); tick(dut); dut->eval();
    check("fault_valid",   dut->o_valid, 1);
    check("fault_pending", dut->o_fault_pending, 1);
    check("fault_vec",     dut->o_fault_vec, 3);

    // ════════════════════════════════════════════════════════════
    // Branch resolution: taken decision + redirect target.
    // o_branch_taken / o_branch_target are combinational, so most checks
    // need no clock edge; the link-value (o_result) checks tick to latch
    // the EX/MEM register.
    // ════════════════════════════════════════════════════════════

    // Unconditional B (cond=AL): always redirects to PC + offset, which
    // the ALU produced as its result.
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_AL; dut->i_alu_op = ALU_ADD;
    dut->i_op_a = 0xFFFF0040; dut->i_op_b = 0x40;   // target = 0xFFFF0080
    dut->i_valid = 1;
    dut->eval();
    check("b_al_taken",  dut->o_branch_taken, 1);
    check("b_al_target", dut->o_branch_target, 0xFFFF0080);

    // Conditional BEQ with Z=1 → taken.
    flush_bubble(dut);
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_EQ; dut->i_alu_op = ALU_ADD;
    dut->i_op_a = 0xFFFF0100; dut->i_op_b = 0x20;    // target = 0xFFFF0120
    dut->i_sr_flags = (1 << FZ);                     // Z=1
    dut->i_valid = 1;
    dut->eval();
    check("beq_z1_taken",  dut->o_branch_taken, 1);
    check("beq_z1_target", dut->o_branch_target, 0xFFFF0120);

    // Conditional BEQ with Z=0 → not taken (no redirect).
    flush_bubble(dut);
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_EQ; dut->i_alu_op = ALU_ADD;
    dut->i_op_a = 0xFFFF0100; dut->i_op_b = 0x20;
    dut->i_sr_flags = 0;                             // Z=0
    dut->i_valid = 1;
    dut->eval();
    check("beq_z0_not_taken", dut->o_branch_taken, 0);

    // JMP: unconditional, target is the register operand (op_a).
    clear(dut);
    dut->i_op_class = OPC_JMP; dut->i_op_a = 0x12345678; dut->i_valid = 1;
    dut->eval();
    check("jmp_taken",      dut->o_branch_taken, 1);
    check("jmp_target_reg", dut->o_branch_target, 0x12345678);

    // BL: links PC+4 into R13 (result = next_pc) and redirects to PC+offset.
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_BL; dut->i_alu_op = ALU_ADD;
    dut->i_op_a = 0xFFFF0200; dut->i_op_b = 0x80;    // target = 0xFFFF0280
    dut->i_gpr_we = 1; dut->i_phys_dst = PHYS_LR;
    dut->i_next_pc = 0xFFFF0204;                     // link value
    dut->i_valid = 1;
    dut->eval();
    check("bl_taken",  dut->o_branch_taken, 1);
    check("bl_target", dut->o_branch_target, 0xFFFF0280);
    tick(dut); dut->eval();
    check("bl_links_nextpc", dut->o_result, 0xFFFF0204);
    check("bl_gpr_we",       dut->o_gpr_we, 1);
    check("bl_phys_dst",     dut->o_phys_dst, PHYS_LR);

    // JALR: links PC+4 into R13 and redirects to the register operand.
    clear(dut);
    dut->i_op_class = OPC_JMP; dut->i_op_a = 0xCAFE0000;
    dut->i_gpr_we = 1; dut->i_phys_dst = PHYS_LR;
    dut->i_next_pc = 0xFFFF0304;
    dut->i_valid = 1;
    dut->eval();
    check("jalr_taken",      dut->o_branch_taken, 1);
    check("jalr_target_reg", dut->o_branch_target, 0xCAFE0000);
    tick(dut); dut->eval();
    check("jalr_links_nextpc", dut->o_result, 0xFFFF0304);

    // A faulting branch slot does not steer the front-end (the fault
    // redirect is taken at WB instead).
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_AL;
    dut->i_op_a = 0xFFFF0040; dut->i_op_b = 0x40;
    dut->i_valid = 1; dut->i_fault_pending = 1; dut->i_fault_vec = 2;
    dut->eval();
    check("branch_fault_no_redirect", dut->o_branch_taken, 0);

    // A squashed branch slot does not steer the front-end.
    clear(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = COND_AL;
    dut->i_op_a = 0xFFFF0040; dut->i_op_b = 0x40;
    dut->i_valid = 1; dut->i_bubble = 1;
    dut->eval();
    check("branch_bubble_no_redirect", dut->o_branch_taken, 0);

    // A non-control-transfer op never redirects.
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_op_a = 5; dut->i_op_b = 3; dut->i_valid = 1;
    dut->eval();
    check("alu_no_redirect", dut->o_branch_taken, 0);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_ex_stage: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

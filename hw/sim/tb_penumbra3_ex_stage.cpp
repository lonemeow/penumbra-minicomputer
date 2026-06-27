// Verilator testbench for penumbra3_ex_stage (via penumbra3_ex_stage_test).
//
// The wrapper decodes a raw instruction word into the bundle EX consumes, so
// this drives instructions and checks EX's job, with the operand-forwarding
// network as the focus:
//   - a clean ALU op computes and latches the EX/MEM1 register
//   - each forward source resolves a dependency: MEM2->EX and WB->EX (flat
//     ports), and EX->EX self-feedback (a producer latched the prior cycle)
//   - the youngest producer wins when two name the same register
//   - a load/sysreg producer is NOT forwarded from MEM1 (its value is not yet
//     computed there) -- the dependent reads the stale value instead
//   - a store forwards its data register even though op_b is the EA immediate
//   - branch resolution: a conditional branch against forwarded flags, and JMP
//   - the divmul peer unit launches, holds EX, and latches both result halves
//   - the handshake: i_stall_in holds EX/MEM1, i_bubble flushes it

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_ex_stage_test.h"
#include "verilated.h"

// op_class enumerators (penumbra3_pkg)
enum { OPC_ALU = 0, OPC_STORE = 2, OPC_DIVMUL = 5 };
// Format-R opcodes (penumbra_pkg)
enum { OP_R_ADD = 0, OP_R_MOV = 8, OP_R_MUL = 16 };
// Format-L opcodes
enum { OP_L_JMP = 11 };
enum { COND_EQ = 1 };
enum { MEM_SZ_WORD = 2 };
enum { VEC_ARITH = 10 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_ex_stage_test* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
}
static uint32_t enc_m(int L, int sz, int se, int rd, int rb, uint16_t off) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22) | ((rb & 0xF) << 18) | ((uint32_t)off << 2);
}
static uint32_t enc_l(int op, int rd) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22);
}
static uint32_t enc_b(int cond, uint32_t off22) {
    return (3u << 30) | ((cond & 0xF) << 26) | ((off22 & 0x3FFFFF) << 4);
}

// Reset every driven input to a benign idle (no forwarding, no stall).
static void clear(Vpenumbra3_ex_stage_test* dut) {
    dut->i_ir = 0;
    dut->i_op_a = 0; dut->i_op_b = 0; dut->i_store_data = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_we = 0;
    dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_we = 0;
    dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
    dut->i_fault_pending = 0; dut->i_fault_vec = 0; dut->i_fault_status = 0;
    dut->i_phys_src_a = 0; dut->i_src_a_fwdable = 0;
    dut->i_phys_src_b = 0; dut->i_src_b_fwdable = 0;
    dut->i_mem2_result = 0; dut->i_mem2_phys_dst = 0;
    dut->i_mem2_dst_we = 0; dut->i_mem2_valid = 0;
    dut->i_wb_result = 0; dut->i_wb_phys_dst = 0;
    dut->i_wb_dst_we = 0; dut->i_wb_valid = 0;
    dut->i_sr_flags = 0; dut->i_mem2_flags = 0; dut->i_mem2_writes_flags = 0;
    dut->i_wb_flags = 0; dut->i_wb_writes_flags = 0; dut->i_sr_committed = 0;
    dut->i_irq_inject = 0; dut->i_irq_vec = 0;
    dut->i_stall_in = 0; dut->i_wb_active = 0; dut->i_bubble = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_ex_stage_test* dut = new Vpenumbra3_ex_stage_test;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_valid", dut->o_mem1_valid, 0);

    // ── ALU passthrough: compute + latch, no forwarding ──────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 2, 0);     // ADD R1, R2
    dut->i_op_a = 0x100; dut->i_op_b = 0x200;
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 2;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("alu_valid",  dut->o_mem1_valid, 1);
    check("alu_class",  dut->o_mem1_op_class, OPC_ALU);
    check("alu_result", dut->o_mem1_result, 0x300);
    check("alu_dst",    dut->o_mem1_phys_dst, 1);
    check("alu_dst_we", dut->o_mem1_phys_dst_we, 1);

    // ── MEM2->EX forward (src_b) ─────────────────────────────────
    // ADD R1, R5 with a stale R5 read; MEM2 carries the real R5 value.
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 5, 0);
    dut->i_op_a = 0x10; dut->i_op_b = 0xBAD;   // 0xBAD = stale regfile read
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 5; dut->i_src_b_fwdable = 1;
    dut->i_mem2_valid = 1; dut->i_mem2_dst_we = 1;
    dut->i_mem2_phys_dst = 5; dut->i_mem2_result = 0x1234;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("mem2_fwd", dut->o_mem1_result, 0x10 + 0x1234);

    // ── WB->EX forward (src_b) ───────────────────────────────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 5, 0);
    dut->i_op_a = 0x10; dut->i_op_b = 0xBAD;
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 5; dut->i_src_b_fwdable = 1;
    dut->i_wb_valid = 1; dut->i_wb_dst_we = 1;
    dut->i_wb_phys_dst = 5; dut->i_wb_result = 0x5678;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("wb_fwd", dut->o_mem1_result, 0x10 + 0x5678);

    // ── MEM2->EX forward (src_a) ─────────────────────────────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 3, 4, 0);      // ADD R3, R4
    dut->i_op_a = 0xAAA; dut->i_op_b = 0x11;    // stale src_a; src_b not forwarded
    dut->i_phys_src_a = 3; dut->i_src_a_fwdable = 1; dut->i_phys_src_b = 4;
    dut->i_mem2_valid = 1; dut->i_mem2_dst_we = 1;
    dut->i_mem2_phys_dst = 3; dut->i_mem2_result = 0x2222;
    dut->i_phys_dst = 3; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("srca_fwd", dut->o_mem1_result, 0x2222 + 0x11);

    // ── Youngest wins: MEM2 (younger) beats WB (older) ───────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 5, 0);
    dut->i_op_a = 0x10; dut->i_op_b = 0xBAD;
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 5; dut->i_src_b_fwdable = 1;
    dut->i_mem2_valid = 1; dut->i_mem2_dst_we = 1;
    dut->i_mem2_phys_dst = 5; dut->i_mem2_result = 0x1234;   // younger
    dut->i_wb_valid = 1; dut->i_wb_dst_we = 1;
    dut->i_wb_phys_dst = 5; dut->i_wb_result = 0x5678;       // older
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("youngest_wins", dut->o_mem1_result, 0x10 + 0x1234);

    // ── EX->EX self-forward (leg 0): producer then consumer ──────
    clear(dut);
    dut->i_ir = enc_r(OP_R_MOV, 6, 0, 0);      // MOV R6, <op_b>
    dut->i_op_b = 0x600D;                       // the moved value
    dut->i_phys_dst = 6; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut);                                  // EX/MEM1 now holds R6 = 0x600D
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 6, 0);      // ADD R1, R6 (reads R6)
    dut->i_op_a = 0x10; dut->i_op_b = 0xBAD;    // stale R6 read
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 6; dut->i_src_b_fwdable = 1;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("exex_self_fwd", dut->o_mem1_result, 0x10 + 0x600D);

    // ── Load exclusion: a LOAD in MEM1 is NOT forwardable ────────
    // Its value is unresolved at MEM1 (only the EA is computed), so the
    // dependent must read the stale value, not the EA.
    clear(dut);
    dut->i_ir = enc_m(1, MEM_SZ_WORD, 0, 7, 5, 0);  // LDW R7, [R5]
    dut->i_op_a = 0x1000; dut->i_op_b = 0;           // EA = 0x1000
    dut->i_phys_dst = 7; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut);                                       // EX/MEM1 holds the LOAD
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 7, 0);            // ADD R1, R7 (reads R7)
    dut->i_op_a = 0x10; dut->i_op_b = 0xBAD;         // stale -- must be used
    dut->i_phys_src_a = 1; dut->i_phys_src_b = 7; dut->i_src_b_fwdable = 1;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("load_excluded", dut->o_mem1_result, 0x10 + 0xBAD);

    // ── Store-data forward: src_b forwards even though op_b is imm ─
    clear(dut);
    dut->i_ir = enc_m(0, MEM_SZ_WORD, 0, 9, 4, 0);   // STW R9, [R4]
    dut->i_op_a = 0x4000; dut->i_op_b = 0;            // EA = base + offset
    dut->i_store_data = 0xDEAD;                       // stale R9 read
    dut->i_phys_src_b = 9; dut->i_src_b_fwdable = 0;  // op_b is the EA immediate
    dut->i_mem2_valid = 1; dut->i_mem2_dst_we = 1;
    dut->i_mem2_phys_dst = 9; dut->i_mem2_result = 0x9999;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("store_class",      dut->o_mem1_op_class, OPC_STORE);
    check("store_data_fwd",   dut->o_mem1_store_data, 0x9999);
    check("store_ea",         dut->o_mem1_result, 0x4000);

    // ── Branch: not taken on Z=0, taken on forwarded Z=1 ─────────
    clear(dut);
    dut->i_ir = enc_b(COND_EQ, 1);             // BEQ +1 word (offset 4)
    dut->i_pc = 0x2000; dut->i_op_a = 0x2000; dut->i_op_b = 4;  // target = PC + 4
    dut->i_valid = 1; dut->eval();
    check("br_nottaken", dut->o_branch_taken, 0);     // Z=0 (no flags in flight)
    dut->i_mem2_flags = 0x2; dut->i_mem2_writes_flags = 1;  // Z=1 (bit 1)
    dut->eval();
    check("br_taken",  dut->o_branch_taken, 1);
    check("br_target", dut->o_branch_target, 0x2004);

    // ── JMP: redirect to op_a ────────────────────────────────────
    clear(dut);
    dut->i_ir = enc_l(OP_L_JMP, 4);            // JMP R4
    dut->i_op_a = 0x3000; dut->i_phys_src_a = 4;
    dut->i_valid = 1; dut->eval();
    check("jmp_taken",  dut->o_branch_taken, 1);
    check("jmp_target", dut->o_branch_target, 0x3000);

    // ── divmul: launch, hold, latch both halves ──────────────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_MUL, 1, 2, 0);      // MUL R1, R2
    dut->i_op_a = 6; dut->i_op_b = 7;          // 6 * 7 = 42
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    check("mul_funit_stall", dut->o_funit_stall, 1);   // busy on the start cycle
    int done = 0;
    for (int i = 0; i < 60 && !done; i++) {
        tick(dut); dut->eval();
        if (dut->o_mem1_valid && dut->o_mem1_op_class == OPC_DIVMUL) done = 1;
    }
    check("mul_done",   done, 1);
    check("mul_lo",     dut->o_mem1_result, 42);
    check("mul_hi",     dut->o_mem1_result_aux, 0);

    // ── Handshake: stall holds EX/MEM1, bubble flushes it ────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 2, 0);
    dut->i_op_a = 0x1; dut->i_op_b = 0x2;
    dut->i_phys_dst = 1; dut->i_phys_dst_we = 1;
    dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("hs_valid", dut->o_mem1_valid, 1);
    dut->i_stall_in = 1; dut->i_valid = 0; dut->eval();
    tick(dut); dut->eval();
    check("hs_held", dut->o_mem1_valid, 1);            // held under stall
    dut->i_stall_in = 0; dut->i_bubble = 1; dut->eval();
    tick(dut); dut->eval();
    check("hs_flushed", dut->o_mem1_valid, 0);         // bubble clears it

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

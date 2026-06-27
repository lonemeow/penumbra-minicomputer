// Verilator testbench for penumbra3_id_stage (via penumbra3_id_stage_test).
//
// The wrapper decodes a raw instruction word into the bundle ID consumes, so
// this drives instructions and checks ID's job:
//   - a clean ALU op issues, drives the regfile read indices, selects the
//     operands, and latches the ID/EX register
//   - a source pending in the scoreboard blocks issue (o_local_stall), and a
//     matching forward broadcast unblocks it
//   - a privileged op faults in user mode (priv_op & ~supervisor -> VEC_PRIV)
//     but not in supervisor mode; an undefined opcode faults VEC_ILLEGAL --
//     both still advance (faults are taken at WB)
//   - the handshake: i_stall_in holds ID/EX, i_bubble flushes it

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_id_stage_test.h"
#include "verilated.h"

enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_WRSYS = 9 };
enum { ALU_ADD = 0 };
enum { VEC_PRIV = 4, VEC_ILLEGAL = 7 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_id_stage_test* dut) {
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

static void clear(Vpenumbra3_id_stage_test* dut) {
    dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
    dut->i_if_fault_pending = 0; dut->i_if_fault_vec = 0; dut->i_if_fault_status = 0;
    dut->i_supervisor = 1;
    dut->i_stall_in = 0; dut->i_bubble = 0; dut->i_fetch_busy = 0; dut->i_pipe_hold = 0;
    dut->i_rd_data_a = 0; dut->i_rd_data_b = 0; dut->i_spr_src_value = 0;
    dut->i_clr_en = 0; dut->i_clr_idx = 0;
    dut->i_fwd0_en = 0; dut->i_fwd0_idx = 0; dut->i_fwd1_en = 0; dut->i_fwd1_idx = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_id_stage_test* dut = new Vpenumbra3_id_stage_test;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_valid", dut->o_ex_valid, 0);

    // ── ALU issue: read indices, operands, ID/EX latch ───────────
    clear(dut);
    dut->i_ir = enc_r(ALU_ADD, 1, 2, 0);   // ADD R1, R2 (R1 = dest + srcA, R2 = srcB)
    dut->i_pc = 0x1000; dut->i_next_pc = 0x1004; dut->i_valid = 1;
    dut->i_rd_data_a = 0xAAAA0001; dut->i_rd_data_b = 0xBBBB0002;
    dut->eval();
    check("alu_rd_a",   dut->o_rd_idx_a, 1);
    check("alu_rd_b",   dut->o_rd_idx_b, 2);
    check("alu_deq",    dut->o_deq_ready, 1);     // issues
    check("alu_nostall", dut->o_local_stall, 0);
    tick(dut); dut->eval();
    check("alu_valid",  dut->o_ex_valid, 1);
    check("alu_class",  dut->o_ex_op_class, OPC_ALU);
    check("alu_aluop",  dut->o_ex_alu_op, ALU_ADD);
    check("alu_op_a",   dut->o_ex_op_a, 0xAAAA0001);
    check("alu_op_b",   dut->o_ex_op_b, 0xBBBB0002);
    check("alu_dst",    dut->o_ex_phys_dst, 1);
    check("alu_dst_we", dut->o_ex_phys_dst_we, 1);
    check("alu_pc",     dut->o_ex_pc, 0x1000);
    check("alu_nofault", dut->o_ex_fault_pending, 0);

    // ── Scoreboard stall + forward unblock ───────────────────────
    // A LOAD into R3 (pending-class) sets the scoreboard bit at issue.
    clear(dut);
    dut->i_ir = enc_m(1, 2, 0, 3, 5, 0);   // LDW R3, [R5+0]
    dut->i_valid = 1; dut->eval();
    check("ld_deq", dut->o_deq_ready, 1);          // LOAD issues
    tick(dut);                                      // scoreboard[R3] set
    // Now a dependent ADD R4, R3 reads R3 (pending) -> blocked.
    clear(dut);
    dut->i_ir = enc_r(ALU_ADD, 4, 3, 0);   // ADD R4, R3 (R3 = srcB)
    dut->i_valid = 1; dut->eval();
    check("dep_blocked_deq",   dut->o_deq_ready, 0);
    check("dep_blocked_stall", dut->o_local_stall, 1);
    // A forward broadcast naming R3 unblocks it.
    dut->i_fwd0_en = 1; dut->i_fwd0_idx = 3; dut->eval();
    check("dep_fwd_deq",   dut->o_deq_ready, 1);
    check("dep_fwd_stall", dut->o_local_stall, 0);

    // ── Privileged op: faults in user mode, not in supervisor ────
    clear(dut);
    dut->i_ir = enc_r(23, 7, 0, 0);        // WRSYS R7 (Format-R op 23)
    dut->i_valid = 1; dut->i_supervisor = 0;   // user mode
    dut->eval();
    check("priv_deq", dut->o_deq_ready, 1);        // fault slot still advances
    tick(dut); dut->eval();
    check("priv_class",  dut->o_ex_op_class, OPC_WRSYS);
    check("priv_fault",  dut->o_ex_fault_pending, 1);
    check("priv_vec",    dut->o_ex_fault_vec, VEC_PRIV);
    // Supervisor mode: same op, no fault.
    clear(dut);
    dut->i_ir = enc_r(23, 7, 0, 0); dut->i_valid = 1; dut->i_supervisor = 1;
    dut->eval(); tick(dut); dut->eval();
    check("sup_nofault", dut->o_ex_fault_pending, 0);

    // ── Undefined opcode: VEC_ILLEGAL ────────────────────────────
    clear(dut);
    dut->i_ir = enc_r(12, 1, 2, 0);        // reserved Format-R opcode
    dut->i_valid = 1; dut->eval();
    check("ill_deq", dut->o_deq_ready, 1);
    tick(dut); dut->eval();
    check("ill_fault", dut->o_ex_fault_pending, 1);
    check("ill_vec",   dut->o_ex_fault_vec, VEC_ILLEGAL);

    // ── Handshake: stall holds, bubble flushes ───────────────────
    clear(dut);
    dut->i_ir = enc_r(ALU_ADD, 1, 2, 0); dut->i_valid = 1; dut->eval();
    tick(dut); dut->eval();
    check("hs_valid", dut->o_ex_valid, 1);         // a valid slot is in ID/EX
    dut->i_stall_in = 1; dut->i_valid = 0; dut->eval();
    check("hs_stall_deq", dut->o_deq_ready, 0);    // does not consume under stall
    tick(dut); dut->eval();
    check("hs_held", dut->o_ex_valid, 1);          // ID/EX holds its slot
    dut->i_stall_in = 0; dut->i_bubble = 1; dut->eval();
    tick(dut); dut->eval();
    check("hs_flushed", dut->o_ex_valid, 0);       // bubble clears ID/EX

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

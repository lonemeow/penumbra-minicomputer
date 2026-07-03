// Verilator testbench for penumbra2_id_stage.
//
// Drives the IF2/ID input, the (external) regfile read data, and the
// downstream scoreboard writer inputs across clock edges, checking the
// ID/EX register and the back-pressure output against the ID-stage
// contract in doc/internals/penumbra2/{pipeline-stages,hazard-model}.md:
//   - reset clears valid
//   - a clean instruction issues: bundle + operands latched into ID/EX
//   - ID does the full operand select (PC for a branch, immediate for
//     an imm op, store data on the raw port-B read)
//   - a RAW hazard stalls issue (downstream writer, and the producer
//     this stage just issued into EX — the internal self-feedback)
//   - a downstream stall holds ID/EX; a flush bubbles it
//   - a faulted slot issues inert (does not scoreboard-stall)

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_id_stage.h"

// op_class / alu_op / mem_op (penumbra2_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_BRANCH = 3 };
enum { ALU_ADD = 0, ALU_PASS = 8 };
enum { MEM_STORE = 2 };
// Format R/L opcodes
enum { OP_R_ADD = 0 };
enum { OP_L_ADDI = 3 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
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

static void tick(Vpenumbra2_id_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Reset the per-cycle control inputs to a quiet baseline.
static void clear(Vpenumbra2_id_stage* dut) {
    dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0;
    dut->i_valid = 0; dut->i_fault_pending = 0; dut->i_fault_vec = 0;
    dut->i_supervisor = 0;
    dut->i_stall_in = 0; dut->i_bubble = 0;
    dut->i_rd_data_a = 0; dut->i_rd_data_b = 0;
    dut->i_mem_dst = 0; dut->i_mem_dst_en = 0;
    dut->i_wb_dst = 0; dut->i_wb_dst_en = 0;
    dut->i_aux_dst = 0; dut->i_aux_dst_en = 0;
}

int main() {
    Vpenumbra2_id_stage* dut = new Vpenumbra2_id_stage;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_valid_low", dut->o_valid, 0);

    // ── Clean issue: ADD R1, R2 (Rd=R1 dst+srcA, Rs=R2 srcB) ─────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 2, 0);
    dut->i_valid = 1;
    dut->i_rd_data_a = 0xAAAA0000;   // R1
    dut->i_rd_data_b = 0xBBBB0000;   // R2
    dut->eval();
    check("add_rdidx_a", dut->o_rd_idx_a, 1);
    check("add_rdidx_b", dut->o_rd_idx_b, 2);
    check("add_no_stall", dut->o_local_stall, 0);
    tick(dut); dut->eval();
    check("add_issued_valid", dut->o_valid, 1);
    check("add_op_class", dut->o_op_class, OPC_ALU);
    check("add_alu_op",   dut->o_alu_op, ALU_ADD);
    check("add_op_a",     dut->o_op_a, 0xAAAA0000);
    check("add_op_b",     dut->o_op_b, 0xBBBB0000);
    check("add_phys_dst", dut->o_phys_dst, 1);
    check("add_gpr_we",   dut->o_gpr_we, 1);
    check("add_fault",    dut->o_fault_pending, 0);

    // ── Self-feedback RAW: consumer reads the just-issued producer ─
    // ID/EX now holds ADD R1 (writes R1), so a consumer reading R1
    // must stall via the EX-dst feedback into the scoreboard.
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 4, 1, 0);   // ADD R4, R1 → reads R1 (srcB)
    dut->i_valid = 1;
    dut->eval();
    check("selffeedback_stall", dut->o_local_stall, 1);
    tick(dut); dut->eval();
    check("selffeedback_bubble", dut->o_valid, 0);   // did not issue

    // ── Operand mux: immediate (ADDI R3, #0x55) ──────────────────
    clear(dut);
    dut->i_ir = enc_l(OP_L_ADDI, 3, 0x0055);
    dut->i_valid = 1;
    dut->i_rd_data_a = 0x12340000;   // R3
    dut->eval();
    tick(dut); dut->eval();
    check("addi_op_a_is_rd", dut->o_op_a, 0x12340000);
    check("addi_op_b_is_imm", dut->o_op_b, 0x00000055);
    check("addi_phys_dst", dut->o_phys_dst, 3);

    // ── Operand mux: branch uses PC for operand A ────────────────
    clear(dut);
    dut->i_ir = enc_b(0 /*AL*/, 0);
    dut->i_pc = 0xFFFF0040;
    dut->i_valid = 1;
    dut->eval();
    tick(dut); dut->eval();
    check("branch_op_class", dut->o_op_class, OPC_BRANCH);
    check("branch_op_a_is_pc", dut->o_op_a, 0xFFFF0040);

    // ── Store: store_data takes the raw port-B read ──────────────
    clear(dut);
    dut->i_ir = enc_m(0 /*store*/, 2 /*word*/, 0, 5 /*data*/, 6 /*base*/, 0);
    dut->i_valid = 1;
    dut->i_rd_data_a = 0x6BA5E000;   // R6 base
    dut->i_rd_data_b = 0x57047A00;   // R5 store data
    dut->eval();
    check("store_rdidx_a", dut->o_rd_idx_a, 6);
    check("store_rdidx_b", dut->o_rd_idx_b, 5);
    tick(dut); dut->eval();
    check("store_op_class", dut->o_op_class, OPC_STORE);
    check("store_mem_op",   dut->o_mem_op, MEM_STORE);
    check("store_op_a_base", dut->o_op_a, 0x6BA5E000);
    check("store_data_portb", dut->o_store_data, 0x57047A00);

    // ── Downstream RAW: a MEM-stage writer blocks a reader ───────
    clear(dut);
    dut->i_mem_dst = 7; dut->i_mem_dst_en = 1;
    dut->i_ir = enc_r(OP_R_ADD, 8, 7, 0);   // reads R7 (busy in MEM)
    dut->i_valid = 1;
    dut->eval();
    check("memdst_raw_stall", dut->o_local_stall, 1);
    tick(dut); dut->eval();
    check("memdst_raw_bubble", dut->o_valid, 0);
    // Writer drains → issues next cycle.
    dut->i_mem_dst_en = 0;
    dut->i_ir = enc_r(OP_R_ADD, 8, 7, 0);
    dut->i_valid = 1;
    dut->eval();
    check("memdst_drained_no_stall", dut->o_local_stall, 0);
    tick(dut); dut->eval();
    check("memdst_drained_issues", dut->o_valid, 1);

    // ── Downstream stall holds ID/EX ─────────────────────────────
    // Issue an instruction, then assert i_stall_in and confirm ID/EX
    // is held. o_local_stall stays low: it reports only the scoreboard
    // interlock — the spine ORs it with the downstream stalls to form
    // the back-pressure to IF.
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 2, 3, 0);
    dut->i_valid = 1;
    dut->eval();
    tick(dut); dut->eval();
    check("held_issued", dut->o_valid, 1);
    uint32_t held_dst = dut->o_phys_dst;
    dut->i_stall_in = 1;
    dut->i_ir = enc_r(OP_R_ADD, 9, 10, 0);   // a different insn knocking
    dut->eval();
    check("stall_in_no_local_stall", dut->o_local_stall, 0);
    tick(dut); dut->eval();
    check("stall_in_holds_valid", dut->o_valid, 1);
    check("stall_in_holds_dst", dut->o_phys_dst, held_dst);

    // ── i_bubble forces a bubble into ID/EX ──────────────────────
    clear(dut);
    dut->i_ir = enc_r(OP_R_ADD, 1, 2, 0);
    dut->i_valid = 1;
    dut->i_bubble = 1;
    dut->eval();
    tick(dut); dut->eval();
    check("bubble_forces_bubble", dut->o_valid, 0);

    // ── Faulted slot issues inert (no scoreboard stall) ──────────
    // An IF-faulted insn whose source would otherwise be busy must
    // still issue (it faults at WB), not RAW-stall on garbage.
    clear(dut);
    dut->i_mem_dst = 5; dut->i_mem_dst_en = 1;   // R5 busy
    dut->i_ir = enc_r(OP_R_ADD, 1, 5, 0);        // would read R5
    dut->i_valid = 1;
    dut->i_fault_pending = 1; dut->i_fault_vec = 2;
    dut->eval();
    check("fault_no_stall", dut->o_local_stall, 0);
    tick(dut); dut->eval();
    check("fault_issues", dut->o_valid, 1);
    check("fault_pending_set", dut->o_fault_pending, 1);
    check("fault_vec", dut->o_fault_vec, 2);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_id_stage: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

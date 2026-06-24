// Verilator testbench for the Penumbra/2.5 penumbra2_ex_stage fork — the BTFN
// branch-resolution behavior the base gen2 testbench can't reach (its EX stage
// has no i_predicted_taken).
//
// The gen2.5 EX stage confirms or corrects the ID-stage prediction:
//   mispredict      = branch_redirect ^ i_predicted_taken
//   redirect_target = branch_redirect ? branch_target : i_next_pc  (fall-through)
//   o_branch_taken  = mispredict & i_valid & ~i_fault_pending & ~i_bubble
// This drives the four predict x actual combinations (the truth table) and the
// three guard conditions with i_predicted_taken=1 — the cases that only became
// load-bearing once prediction injected redirect-intent into otherwise-dead
// slots, and which no program-level test in the suite reaches.
//
// NZCV bundle packs as SR[3:0]: N=bit0 Z=bit1 C=bit2 V=bit3.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_ex_stage.h"

enum { OPC_ALU = 0, OPC_BRANCH = 3, OPC_JMP = 4 };
enum { ALU_ADD = 0 };
enum { MEM_NONE = 0 };
enum { COND_AL = 0, COND_EQ = 1 };   // penumbra_pkg COND_*
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
static void clear(Vpenumbra2_ex_stage* dut) {
    dut->i_op_class = OPC_ALU; dut->i_alu_op = ALU_ADD; dut->i_divmul_op = 0;
    dut->i_op_a = 0; dut->i_op_b = 0; dut->i_store_data = 0;
    // GPR forwarding tied off: this tb drives EX in isolation, where the EX/MEM
    // forward source is the stage's own multi-cycle self-feedback. Forwarding is
    // covered end-to-end at integration (isa/test_forward.s); here it stays
    // inert so the branch-resolution checks below match the gen2 baseline.
    dut->i_phys_src_a = 0; dut->i_fwd_a_en = 0;
    dut->i_phys_src_b = 0; dut->i_fwd_b_en = 0;
    dut->i_first_cycle = 1; // each driven slot is on its first EX cycle, so the
                            // (tied-off) forward fallback tracks the driven operand
    dut->i_wb_fwd_dst = 0; dut->i_wb_fwd_value = 0; dut->i_wb_fwd_valid = 0;
    dut->i_cond = COND_AL; dut->i_predicted_taken = 0;
    dut->i_mem_op = MEM_NONE; dut->i_mem_size = 0; dut->i_sign_ext = 0;
    dut->i_sys_dev = 0; dut->i_sys_reg = 0; dut->i_spr_sel = 0;
    dut->i_drain_commit = 0; dut->i_post_commit_wait = 0;
    dut->i_gpr_we = 0; dut->i_spr_we = 0; dut->i_flag_we = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_en = 0;
    dut->i_pc = 0; dut->i_next_pc = 0; dut->i_is_trap = 0; dut->i_valid = 0;
    dut->i_fault_pending = 0; dut->i_fault_vec = 0; dut->i_fault_status = 0;
    dut->i_irq_inject = 0; dut->i_irq_vec = 0;
    dut->i_sr_flags = 0; dut->i_wb_flags = 0; dut->i_wb_writes_flags = 0;
    dut->i_sr_committed = 0;
    dut->i_stall_in = 0; dut->i_wb_active = 0; dut->i_bubble = 0;
}
// Drive a bubble through EX/MEM so the flag-bypass MEM self-feedback leg is
// empty; a conditional branch's forwarded NZCV is then i_sr_flags alone.
static void flush_bubble(Vpenumbra2_ex_stage* dut) {
    clear(dut); dut->i_valid = 0; dut->eval(); tick(dut); dut->eval();
}
// Set up a Format-B branch slot: target = op_a+op_b, fall-through = next_pc,
// actual-taken controlled by the Z flag against COND_EQ.
static void setup_branch(Vpenumbra2_ex_stage* dut, int cond, int zflag,
                         int predicted, int valid) {
    flush_bubble(dut);
    dut->i_op_class = OPC_BRANCH; dut->i_cond = cond;
    dut->i_op_a = 0xFFFF0040; dut->i_op_b = 0x40;   // branch target = 0xFFFF0080
    dut->i_next_pc = 0xFFFF0044;                     // fall-through
    dut->i_sr_flags = zflag ? (1u << FZ) : 0;
    dut->i_predicted_taken = predicted;
    dut->i_valid = valid;
}

int main() {
    Vpenumbra2_ex_stage* dut = new Vpenumbra2_ex_stage;
    dut->i_rst = 1; clear(dut); dut->eval(); tick(dut); tick(dut); dut->i_rst = 0;

    const uint32_t TARGET = 0xFFFF0080, FALLTHRU = 0xFFFF0044;

    // ── Mispredict truth table: EX redirects iff its resolution disagrees
    //    with the ID guess. ──
    // pred-taken, actual-taken (COND_EQ, Z=1): correct → EX stays quiet.
    setup_branch(dut, COND_EQ, /*z*/1, /*pred*/1, /*valid*/1); dut->eval();
    check("predT_actualT_quiet", dut->o_branch_taken, 0);

    // pred-taken, actual-not-taken (Z=0): mispredict → redirect to fall-through.
    setup_branch(dut, COND_EQ, 0, 1, 1); dut->eval();
    check("predT_actualNT_redirect",       dut->o_branch_taken, 1);
    check("predT_actualNT_to_fallthrough",  dut->o_branch_target, FALLTHRU);

    // pred-not-taken, actual-taken: mispredict → redirect to the target.
    setup_branch(dut, COND_EQ, 1, 0, 1); dut->eval();
    check("predNT_actualT_redirect",  dut->o_branch_taken, 1);
    check("predNT_actualT_to_target", dut->o_branch_target, TARGET);

    // pred-not-taken, actual-not-taken: collapses to gen2 baseline — no redirect.
    setup_branch(dut, COND_EQ, 0, 0, 1); dut->eval();
    check("predNT_actualNT_quiet", dut->o_branch_taken, 0);

    // ── Guards: a dead slot carrying predicted_taken=1 must NOT steer fetch.
    //    (actual-not-taken so mispredict=1; only the guard prevents a spurious
    //    redirect — the exact case program-level tests can't construct.) ──
    setup_branch(dut, COND_EQ, 0, 1, 1);
    dut->i_fault_pending = 1; dut->i_fault_vec = 2; dut->eval();
    check("pred_fault_no_redirect", dut->o_branch_taken, 0);

    setup_branch(dut, COND_EQ, 0, 1, 1);
    dut->i_bubble = 1; dut->eval();
    check("pred_bubble_no_redirect", dut->o_branch_taken, 0);

    setup_branch(dut, COND_EQ, 0, 1, /*valid*/0); dut->eval();
    check("pred_invalid_no_redirect", dut->o_branch_taken, 0);

    // ── Unpredicted control transfers still resolve in EX. ──
    // Unconditional B, predicted taken correctly → EX quiet.
    setup_branch(dut, COND_AL, 0, 1, 1); dut->eval();
    check("uncond_predT_quiet", dut->o_branch_taken, 0);

    // JMP is never predicted (ID drives predicted_taken=0) → EX redirects to op_a.
    flush_bubble(dut);
    dut->i_op_class = OPC_JMP; dut->i_op_a = 0xCAFE0000;
    dut->i_predicted_taken = 0; dut->i_valid = 1; dut->eval();
    check("jmp_redirect", dut->o_branch_taken, 1);
    check("jmp_target",   dut->o_branch_target, 0xCAFE0000);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

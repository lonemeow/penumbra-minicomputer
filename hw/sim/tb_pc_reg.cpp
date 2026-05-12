// Verilator testbench for the Penumbra PC Unit
//
// Tests:
//   - Reset to address 0
//   - PC holds when i_pc_load=0
//   - PC loads when i_pc_load=1
//   - PC+4 output is always current PC + 4
//   - Branch offset: positive forward branch
//   - Branch offset: negative backward branch
//   - Branch offset: zero offset (branch to next instruction)
//   - Branch offset: maximum positive offset
//   - Branch offset: maximum negative offset
//   - EPC: latched on exception entry
//   - EPC: preserved across normal PC loads
//   - EPC: reset to zero
//   - Full branch sequence: load PC, compute target, take branch
//   - Full exception sequence: snapshot EPC then handler loads new PC

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vpc_reg.h"

static int errors = 0;
static int tests  = 0;

static void tick(Vpc_reg* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

static void clear_inputs(Vpc_reg* dut) {
    dut->i_pc_load      = 0;
    dut->i_pc_next      = 0;
    dut->i_offset22     = 0;
    dut->i_except_entry = 0;
}

static void reset(Vpc_reg* dut) {
    clear_inputs(dut);
    dut->i_rst = 1;
    tick(dut);
    dut->i_rst = 0;
}

// Load a value into PC
static void load_pc(Vpc_reg* dut, uint32_t val) {
    dut->i_pc_load = 1;
    dut->i_pc_next = val;
    tick(dut);
    dut->i_pc_load = 0;
}

// Compute PC + sign_extend(offset22 << 2) in C for verification
static uint32_t expected_branch_target(uint32_t pc, uint32_t offset22) {
    // Sign-extend 22-bit to 32-bit
    int32_t signed_offset = (int32_t)(offset22 << 10) >> 10;  // sign-extend
    // Shift left 2 for byte address
    int32_t byte_offset = signed_offset << 2;
    return pc + (uint32_t)byte_offset;
}

int main(int argc, char** argv) {
    Vpc_reg* dut = new Vpc_reg;

    // ── Reset ──────────────────────────────────────────────────
    // pc_reg's RESET_PC default is 0xFFFF_0000 (boot ROM base);
    // pc_plus4 is the combinational +4 of o_pc.
    reset(dut);
    check("reset_pc",        dut->o_pc,        0xFFFF0000);
    check("reset_pc_plus4",  dut->o_pc_plus4,  0xFFFF0004);
    check("reset_epc", dut->o_epc, 0x00000000);

    // ── PC holds when load=0 ───────────────────────────────────
    dut->i_pc_next = 0xDEADBEEF;
    dut->i_pc_load = 0;
    tick(dut);
    check("hold_pc", dut->o_pc, 0xFFFF0000);

    // ── PC loads when load=1 ───────────────────────────────────
    load_pc(dut, 0x00001000);
    check("load_pc",       dut->o_pc,       0x00001000);
    check("load_pc_plus4", dut->o_pc_plus4, 0x00001004);

    // ── PC+4 tracks PC ─────────────────────────────────────────
    load_pc(dut, 0x0000FFFC);
    check("pc_plus4_wrap", dut->o_pc_plus4, 0x00010000);

    load_pc(dut, 0xFFFFFFFC);
    check("pc_plus4_overflow", dut->o_pc_plus4, 0x00000000);  // wraps

    // ── Branch offset: forward ──────────────────────────────────
    load_pc(dut, 0x00001000);
    // offset22 = 4 → byte offset = 4 << 2 = 16 → target = 0x1000 + 16 = 0x1010
    dut->i_offset22 = 4;
    dut->eval();
    check("branch_fwd", dut->o_pc_offset, expected_branch_target(0x00001000, 4));
    check("branch_fwd_val", dut->o_pc_offset, 0x00001010);

    // ── Branch offset: backward ─────────────────────────────────
    // offset22 = -2 (0x3FFFFE in 22-bit two's complement) → byte = -8 → target = 0x1000 - 8 = 0x0FF8
    uint32_t neg2 = 0x3FFFFE;  // -2 in 22-bit two's complement
    dut->i_offset22 = neg2;
    dut->eval();
    check("branch_bwd", dut->o_pc_offset, expected_branch_target(0x00001000, neg2));
    check("branch_bwd_val", dut->o_pc_offset, 0x00000FF8);

    // ── Branch offset: zero (branch to self = infinite loop) ────
    dut->i_offset22 = 0;
    dut->eval();
    check("branch_zero", dut->o_pc_offset, 0x00001000);  // PC + 0

    // ── Branch offset: +1 (branch to next instruction) ──────────
    // offset22 = 1 → byte = 4 → target = 0x1000 + 4 = 0x1004
    dut->i_offset22 = 1;
    dut->eval();
    check("branch_next", dut->o_pc_offset, 0x00001004);

    // ── Branch offset: max positive ─────────────────────────────
    // offset22 = 0x1FFFFF (largest positive 22-bit) → byte = 0x7FFFFC → target = 0 + 0x7FFFFC
    load_pc(dut, 0x00000000);
    dut->i_offset22 = 0x1FFFFF;
    dut->eval();
    check("branch_max_pos", dut->o_pc_offset, expected_branch_target(0x00000000, 0x1FFFFF));
    check("branch_max_pos_val", dut->o_pc_offset, 0x007FFFFC);

    // ── Branch offset: max negative ─────────────────────────────
    // offset22 = 0x200000 (most negative 22-bit = -2097152) → byte = -8388608 = 0xFF800000
    load_pc(dut, 0x00800000);
    dut->i_offset22 = 0x200000;
    dut->eval();
    check("branch_max_neg", dut->o_pc_offset, expected_branch_target(0x00800000, 0x200000));
    check("branch_max_neg_val", dut->o_pc_offset, 0x00000000);  // 0x800000 + (-0x800000) = 0

    // ── EPC: latched on exception entry ──────────────────────────
    load_pc(dut, 0x00002000);
    clear_inputs(dut);
    dut->i_except_entry = 1;
    tick(dut);
    clear_inputs(dut);
    check("epc_latched", dut->o_epc, 0x00002000);
    // PC itself unchanged (no pc_load)
    check("epc_pc_unchanged", dut->o_pc, 0x00002000);

    // ── EPC: preserved across normal PC loads ───────────────────
    load_pc(dut, 0x00003000);
    check("epc_preserved", dut->o_epc, 0x00002000);
    check("pc_moved", dut->o_pc, 0x00003000);

    // ── EPC: updated only on new exception ──────────────────────
    load_pc(dut, 0x00004000);
    dut->i_except_entry = 1;
    tick(dut);
    clear_inputs(dut);
    check("epc_updated", dut->o_epc, 0x00004000);

    // ── EPC: reset clears it ────────────────────────────────────
    reset(dut);
    check("epc_reset", dut->o_epc, 0x00000000);

    // ── Full branch sequence ────────────────────────────────────
    // Simulate: fetch at 0x1000, branch forward by 10 words
    load_pc(dut, 0x00001000);
    dut->i_offset22 = 10;  // 10 words = 40 bytes forward
    dut->eval();
    uint32_t target = dut->o_pc_offset;
    check("seq_target", target, 0x00001028);  // 0x1000 + 40 = 0x1028

    // Take the branch: load the target into PC
    dut->i_pc_load = 1;
    dut->i_pc_next = target;
    tick(dut);
    clear_inputs(dut);
    check("seq_after_branch", dut->o_pc, 0x00001028);
    check("seq_after_plus4",  dut->o_pc_plus4, 0x0000102C);

    // ── Full exception sequence ─────────────────────────────────
    // User code at 0x2000, exception occurs, handler at 0xFFFF0000
    load_pc(dut, 0x00002000);
    // Exception entry: snapshot PC
    dut->i_except_entry = 1;
    tick(dut);
    clear_inputs(dut);
    check("except_epc", dut->o_epc, 0x00002000);

    // Handler loads via MDR → pc_mux → PC (simulated as direct load)
    load_pc(dut, 0xFFFF0000);
    check("except_handler_pc", dut->o_pc, 0xFFFF0000);
    // EPC still holds user PC
    check("except_epc_kept", dut->o_epc, 0x00002000);

    // RTI: restore PC from stack (simulated as direct load)
    load_pc(dut, 0x00002000);
    check("except_rti_pc", dut->o_pc, 0x00002000);

    // ── Summary ────────────────────────────────────────────────
    printf("pc_reg: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}

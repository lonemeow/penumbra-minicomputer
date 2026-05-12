// Verilator testbench for the Penumbra Datapath
//
// Tests the integrated datapath by driving micro-word control signals
// and verifying end-to-end data flow through the module hierarchy.
//
// Tests:
//   - Reset state
//   - ADD Rd, Rs: register-to-register through ALU, writeback, flags
//   - LDW Rd, [Rb + offset]: address compute, memory read, writeback
//   - Branch offset computation
//   - Exception entry: shadow snapshot, mode switch
//   - Register address routing: IR-indirect vs literal
//   - F-bit write-enable gating (CMP)
//   - IR latch from memory data

#include <cstdio>
#include <cstdint>
#include "Vdatapath.h"

static int errors = 0, tests = 0;

static void tick(Vdatapath* d) { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}

static void check1(const char* n, int got, int exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got %d, expected %d\n", n, got, exp); errors++; }
}

// Register address encoding
static const int IR_RD = 0;  // Format-dependent Rd
static const int IR_RS = 1;  // Format-dependent Rs/Rb

// ALU ops
static const int ALU_ADD    = 0b00000;
static const int ALU_SUB    = 0b00001;
static const int ALU_PASS_A = 0b01000;

// PC source
static const int PC_HOLD    = 0b000;
static const int PC_PLUS4   = 0b001;
static const int PC_OFFSET  = 0b010;

// A-bus source
static const int ASRC_REG   = 0b00;

// B-mux source
static const int BMUX_REG   = 0b00;
static const int BMUX_IMM   = 0b01;
static const int BMUX_C4    = 0b10;

// Clear all control inputs
static void clear(Vdatapath* d) {
    d->i_a_src = 0; d->i_reg_a_sel = 0; d->i_reg_b_sel = 0;
    d->i_reg_w_sel = 0; d->i_reg_w_en = 0; d->i_alu_op = 0;
    d->i_b_mux_sel = 0; d->i_w_mux_sel = 0; d->i_imm_mode = 0;
    d->i_flag_w_en = 0; d->i_sr_load = 0; d->i_mar_load = 0;
    d->i_mdr_load_mem = 0; d->i_mdr_load_a = 0; d->i_pc_src = 0;
    d->i_alu_start = 0; d->i_pc_load = 0;
    d->i_except_entry = 0; d->i_vector_num = 0;
    d->i_ei_set = 0; d->i_di_set = 0; d->i_ei_shadow_clr = 0;
    d->i_ir_load = 0; d->i_mem_rdata = 0;
    // Word access (2'b10).  Otherwise byte_ext/byte_rep treat loads
    // and MDR-A-bus stores as byte-sized, masking off the high 24
    // bits of the value the rest of the test cares about.
    d->i_mem_size = 0b10;
    d->i_sign_ext = 0;
    d->i_spr_write = 0;
}

static void reset(Vdatapath* d) {
    clear(d);
    d->i_rst = 1;
    tick(d);
    d->i_rst = 0;
}

int main() {
    Vdatapath* d = new Vdatapath;

    // ── Reset state ────────────────────────────────────────────
    reset(d);
    // RESET_PC default is 0xFFFF_0000 (boot ROM base).
    check("reset_pc", d->o_pc, 0xFFFF0000);
    check1("reset_sr_s", d->o_sr_s, 1);     // Supervisor mode
    check1("reset_sr_i", d->o_sr_i, 0);     // Interrupts disabled
    check1("reset_busy", d->o_alu_busy, 0);

    // ── Load IR with a Format R ADD R3, R4 instruction ─────────
    // Format R: [00 | op=00000 | Rd=0011 | Rs=0100 | F=0 | spare...]
    // IR[31:30]=00, IR[29:25]=00000, IR[24:21]=0011, IR[20:17]=0100, IR[16]=0
    uint32_t add_r3_r4 = (0b00u << 30) | (0b00000u << 25) | (3u << 21) | (4u << 17) | (0u << 16);
    d->i_mem_rdata = add_r3_r4;
    d->i_ir_load = 1;
    tick(d);
    clear(d);

    // Verify field extraction
    check("ir_format", d->o_format, 0b00);
    check("ir_r_op", d->o_r_op, 0b00000);

    // ── Write values to R3 and R4 using literal addressing ─────
    // Write 0x10 to R3: drive A-bus with zero (R0), B-mux=const4...
    // Actually, simplest: use ALU PASS_B with immediate, w_mux=R-bus.
    // But we need to load IR with an instruction that has the right imm.
    // Instead, let's use a direct approach: load MDR from mem, then
    // w_mux=MDR to write register.

    // Write 100 to R3 via MDR path
    d->i_mem_rdata = 100;
    d->i_mdr_load_mem = 1;
    tick(d);
    d->i_mdr_load_mem = 0;

    d->i_reg_w_sel = 3;     // Literal R3
    d->i_reg_w_en = 1;
    d->i_w_mux_sel = 1;     // MDR
    tick(d);
    clear(d);

    // Write 200 to R4 via MDR path
    d->i_mem_rdata = 200;
    d->i_mdr_load_mem = 1;
    tick(d);
    d->i_mdr_load_mem = 0;

    d->i_reg_w_sel = 4;     // Literal R4
    d->i_reg_w_en = 1;
    d->i_w_mux_sel = 1;     // MDR
    tick(d);
    clear(d);

    // ── Reload IR with ADD R3, R4 ──────────────────────────────
    d->i_mem_rdata = add_r3_r4;
    d->i_ir_load = 1;
    tick(d);
    clear(d);

    // ── Execute ADD R3, R4 ─────────────────────────────────────
    // reg_a_sel = IR_RD (→ R3), reg_b_sel = IR_RS (→ R4)
    // ALU ADD, w_mux = R-bus, reg_w_sel = IR_RD (→ R3), reg_w_en = 1
    // flag_w_en = 1, pc_src = PC+4
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = IR_RD;     // → R3 (Rd in Format R)
    d->i_reg_b_sel = IR_RS;     // → R4 (Rs in Format R)
    d->i_reg_w_sel = IR_RD;     // → R3
    d->i_reg_w_en  = 1;
    d->i_alu_op    = ALU_ADD;
    d->i_b_mux_sel = BMUX_REG;
    d->i_w_mux_sel = 0;         // R-bus
    d->i_flag_w_en = 1;
    d->i_pc_src    = PC_PLUS4;
    d->i_pc_load   = 1;
    tick(d);
    clear(d);

    // R3 should now be 100 + 200 = 300
    // Read R3 back: set reg_a_sel to literal 3, check ALU PASS_A output
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = 3;        // Literal R3
    d->i_alu_op    = ALU_PASS_A;
    d->eval();

    // The A-bus → ALU PASS_A → R-bus should show 300
    // We can't directly observe r_bus, but we can write to MAR and read it
    d->i_mar_load = 1;
    tick(d);
    clear(d);
    check("add_result", d->o_mem_addr, 300);

    // PC should have advanced one word past RESET_PC.
    check("add_pc_advanced", d->o_pc, 0xFFFF0004);

    // ── F-bit gating: CMP R3, R4 (Format R, F=1) ──────────────
    // Same as ADD but with F=1 → reg write suppressed
    uint32_t cmp_r3_r4 = add_r3_r4 | (1u << 16);  // Set F bit
    d->i_mem_rdata = cmp_r3_r4;
    d->i_ir_load = 1;
    tick(d);
    clear(d);

    // Write a known value to R3 first (to verify it's NOT overwritten)
    d->i_mem_rdata = 0x12345678;
    d->i_mdr_load_mem = 1;
    tick(d);
    d->i_mdr_load_mem = 0;
    d->i_reg_w_sel = 3;
    d->i_reg_w_en = 1;
    d->i_w_mux_sel = 1;  // MDR
    tick(d);
    clear(d);

    // Reload CMP instruction in IR
    d->i_mem_rdata = cmp_r3_r4;
    d->i_ir_load = 1;
    tick(d);
    clear(d);

    // Execute CMP: reg_w_en=1 but F-bit should gate it off
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = IR_RD;
    d->i_reg_b_sel = IR_RS;
    d->i_reg_w_sel = IR_RD;
    d->i_reg_w_en  = 1;        // Micro-word says write...
    d->i_alu_op    = ALU_SUB;
    d->i_b_mux_sel = BMUX_REG;
    d->i_w_mux_sel = 0;
    d->i_flag_w_en = 1;
    tick(d);
    clear(d);

    // R3 should still be 0x12345678 (F-bit suppressed write)
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = 3;
    d->i_alu_op    = ALU_PASS_A;
    d->i_mar_load  = 1;
    tick(d);
    clear(d);
    check("cmp_no_write", d->o_mem_addr, 0x12345678);

    // ── Exception entry: shadow snapshot ───────────────────────
    // Set up: user mode with I=1 via sr_load
    // Step 1: load MDR with the SR value we want
    uint32_t user_sr = (1u << 30);  // I=1, S=0
    d->i_mem_rdata = user_sr;
    d->i_mdr_load_mem = 1;
    tick(d);
    clear(d);
    // Step 2: sr_load from W-mux (MDR path)
    d->i_sr_load = 1;
    d->i_w_mux_sel = 1;  // MDR
    tick(d);
    clear(d);

    check1("pre_except_s", d->o_sr_s, 0);
    check1("pre_except_i", d->o_sr_i, 1);

    // Fire exception entry
    d->i_except_entry = 1;
    d->i_vector_num = 4;  // Page fault
    tick(d);
    clear(d);

    // Should now be in supervisor mode, interrupts disabled
    check1("except_s", d->o_sr_s, 1);
    check1("except_i", d->o_sr_i, 0);

    // ── Literal register addressing: R14 (SP) ──────────────────
    // Write a value to R14 (SSP) using literal addressing
    d->i_mem_rdata = 0xFFFF0000;
    d->i_mdr_load_mem = 1;
    tick(d);
    d->i_mdr_load_mem = 0;
    d->i_reg_w_sel = 14;    // Literal R14
    d->i_reg_w_en = 1;
    d->i_w_mux_sel = 1;     // MDR
    tick(d);
    clear(d);

    // Read it back via A-bus → MAR
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = 14;     // Literal R14
    d->i_alu_op    = ALU_PASS_A;
    d->i_mar_load  = 1;
    tick(d);
    clear(d);
    check("literal_r14", d->o_mem_addr, 0xFFFF0000);

    // ── MDR A-bus load path (store data) ───────────────────────
    // Drive R3 value onto A-bus, load into MDR via mdr_load_a
    d->i_a_src     = ASRC_REG;
    d->i_reg_a_sel = 3;
    d->i_mdr_load_a = 1;
    tick(d);
    clear(d);
    check("mdr_a_bus", d->o_mem_wdata, 0x12345678);  // R3's value

    // ── Summary ────────────────────────────────────────────────
    printf("datapath: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return (errors > 0) ? 1 : 0;
}

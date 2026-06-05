// Verilator testbench for penumbra2_spine — the first ID->EX->MEM->WB
// pipeline integration.
//
// A small fetch model drives an instruction stream into the spine, honoring
// o_fetch_stall (hold the current instruction whenever the front-end is
// back-pressured or scoreboard-stalled). A shadow register file mirrors the
// WB commit port (o_commit_*) so the final architectural state can be
// checked.
//
// Why checking the committed *values* is a real hazard test: gen2 has no
// register write-through, so if the scoreboard ever let a dependent
// instruction issue before its producer committed, that instruction would
// read the *old* regfile value and compute a wrong result. Correct final
// values therefore prove the RAW stalls (and the divmul dual-write commit)
// actually fired — no separate cycle-counting needed.
//
// Covers: independent ALU flow, a RAW-dependent ADD chain (Example 1), a MUL
// whose two results (Rd low, Rdh high) are each consumed (Example 5), and
// back-to-back divmuls with a reader of the older divmul's high half (the
// two-aux-live window the spine's aux-mutual-exclusion assertion carves out).
// Straight-line only — no branches/loads/exceptions yet.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_spine.h"

// Format R / L opcodes + alu ops (penumbra2_pkg / instruction-encoding.md)
enum { OP_R_ADD = 0, OP_R_SUB = 1, OP_R_MOV = 8, OP_R_MUL = 16 };
enum { OP_L_LLI = 0 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_spine* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// ── Instruction word encoders (match tb_penumbra2_decode) ────────
static uint32_t enc_r(int op, int rd, int rs, int f, int field1512 = 0) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16) | ((field1512 & 0xF) << 12);
}
static uint32_t enc_l(int op, int rd, uint16_t imm) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22) | imm;
}

// Run a program through the spine, mirroring WB commits into `shadow`
// (indexed by physical register entry). Drains for `cycles` total.
static void run(Vpenumbra2_spine* dut, const uint32_t* prog, int n,
                uint32_t* shadow, int cycles) {
    // Reset (held two cycles, testbench convention).
    dut->i_valid = 0; dut->i_supervisor = 1;
    dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;

    int fidx = 0;
    for (int c = 0; c < cycles; c++) {
        if (fidx < n) {
            dut->i_ir      = prog[fidx];
            dut->i_pc      = 0x1000 + 4 * fidx;
            dut->i_next_pc = 0x1000 + 4 * (fidx + 1);
            dut->i_valid   = 1;
        } else {
            dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
        }
        dut->eval();
        bool stall = dut->o_fetch_stall;
        if (dut->o_commit_we) shadow[dut->o_commit_idx] = dut->o_commit_data;
        tick(dut);
        if (!stall && fidx < n) fidx++;
    }
}

int main() {
    Vpenumbra2_spine* dut = new Vpenumbra2_spine;
    uint32_t sh[22];

    // ── Test 0: independent ALU flow (no hazards) ────────────────
    {
        const uint32_t prog[] = {
            enc_l(OP_L_LLI, 1, 0x11),
            enc_l(OP_L_LLI, 2, 0x22),
            enc_l(OP_L_LLI, 3, 0x33),
        };
        for (auto& v : sh) v = 0;
        run(dut, prog, 3, sh, 32);
        check("flow_r1", sh[1], 0x11);
        check("flow_r2", sh[2], 0x22);
        check("flow_r3", sh[3], 0x33);
    }

    // ── Test 1: RAW-dependent ADD chain (Example 1) ──────────────
    // R1=5, R2=3, R3=16; R1=R1+R2=8; R3=R3+R1 reads the just-produced R1.
    // A missed stall would read R1=5 (the LLI value) → R3=21, not 24.
    {
        const uint32_t prog[] = {
            enc_l(OP_L_LLI, 1, 5),
            enc_l(OP_L_LLI, 2, 3),
            enc_l(OP_L_LLI, 3, 16),
            enc_r(OP_R_ADD, 1, 2, 0),   // R1 = R1 + R2 = 8   (producer)
            enc_r(OP_R_ADD, 3, 1, 0),   // R3 = R3 + R1 = 24  (RAW on R1)
        };
        for (auto& v : sh) v = 0;
        run(dut, prog, 5, sh, 48);
        check("raw_r1", sh[1], 8);
        check("raw_r2", sh[2], 3);
        check("raw_r3", sh[3], 24);
    }

    // ── Test 2: MUL dual-write, both results consumed (Example 5) ─
    // R1=6, R2=7, R4=100, R5=200; MUL R1,R2 (Rdh=R3) → R1=42 (low), R3=0
    // (high). R4=R4+R1 depends on Rd; R5=R5+R3 depends on Rdh — both must
    // stall until the divmul commits both halves.
    {
        const uint32_t prog[] = {
            enc_l(OP_L_LLI, 1, 6),
            enc_l(OP_L_LLI, 2, 7),
            enc_l(OP_L_LLI, 4, 100),
            enc_l(OP_L_LLI, 5, 200),
            enc_r(OP_R_MUL, 1, 2, 0, /*Rdh=*/3),  // R1 = 42, R3 = 0
            enc_r(OP_R_ADD, 4, 1, 0),             // R4 = 100 + 42 = 142  (RAW on Rd)
            enc_r(OP_R_ADD, 5, 3, 0),             // R5 = 200 + 0  = 200  (RAW on Rdh)
        };
        for (auto& v : sh) v = 0;
        run(dut, prog, 7, sh, 128);
        check("mul_r1_lo", sh[1], 42);
        check("mul_r3_hi", sh[3], 0);
        check("mul_r4",    sh[4], 142);
        check("mul_r5",    sh[5], 200);
    }

    // ── Test 3: back-to-back divmuls + reader of the OLDER divmul's Rdh ──
    // As divmul A leaves EX for MEM, divmul B enters EX the same cycle, so for
    // one cycle two aux (Rdh) writers are live — A.Rdh in MEM, B.Rdh in EX.
    // The scoreboard's single aux port exposes only the youngest (B.Rdh); A.Rd
    // is still covered by i_mem_dst, but A.Rdh is not. RAW must still hold
    // because B's ~33-cycle EX stall pins the reader in ID until A commits.
    //   A: MUL R1,R2 (Rdh=R3): R1=42, R3=0 (overwrites R3's 0xAB sentinel)
    //   B: MUL R4,R5 (Rdh=R6): R4=12, R6=0
    //   reader: ADD R7,R3 reads A.Rdh — correct R7=0x100, missed stall=0x1AB
    // (the sentinel makes a hidden-aux RAW miss observable). Also the live
    // exercise of the spine's id_stall-carved aux-mutual-exclusion assertion.
    {
        const uint32_t prog[] = {
            enc_l(OP_L_LLI, 3, 0xAB),             // sentinel in A's Rdh dst
            enc_l(OP_L_LLI, 1, 6),
            enc_l(OP_L_LLI, 2, 7),
            enc_l(OP_L_LLI, 4, 3),
            enc_l(OP_L_LLI, 5, 4),
            enc_l(OP_L_LLI, 7, 0x100),            // reader accumulator base
            enc_r(OP_R_MUL, 1, 2, 0, /*Rdh=*/3),  // A: R1=42, R3=0
            enc_r(OP_R_MUL, 4, 5, 0, /*Rdh=*/6),  // B: R4=12, R6=0
            enc_r(OP_R_ADD, 7, 3, 0),             // reads A.Rdh (R3)
        };
        for (auto& v : sh) v = 0;
        run(dut, prog, 9, sh, 300);
        check("b2b_mulA_lo", sh[1], 42);
        check("b2b_mulA_hi", sh[3], 0);     // sentinel overwritten by A's Rdh
        check("b2b_mulB_lo", sh[4], 12);
        check("b2b_mulB_hi", sh[6], 0);
        check("b2b_raw_rdh", sh[7], 0x100); // 0x1AB would mean a missed aux RAW
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

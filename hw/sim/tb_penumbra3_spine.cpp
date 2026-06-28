// Verilator testbench for penumbra3_spine (via penumbra3_spine_test).
//
// Drives the spine as a pipeline with a fetch-stream model: a program is
// presented at the FIFO head and advanced on o_deq_ready, while commits (the
// regfile write port) and fault-commits are collected. The collected sequences
// are checked against the expected streams -- exercising issue, operand
// forwarding, the scoreboard interlock, the divmul dual-write (the wb_local_stall
// freeze), precise faults, and in-order commit.
//
// Memory ops (loads/stores) and the load_pending freeze are covered in a
// follow-on once the verdict/fill memory model is added; this round drives the
// register datapath, divmul, and faults (no external memory needed).

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vpenumbra3_spine_test.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_spine_test* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Instruction encoders (doc/system/instruction-encoding.md)
static uint32_t enc_l(int op, int rd, uint16_t imm) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22) | imm;
}
static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
}
enum { OP_L_LLI = 0, OP_L_ADDI = 3, OP_R_ADD = 0, OP_R_MUL = 16, OP_R_RESERVED = 12 };
enum { VEC_ILLEGAL = 7 };

struct Commit { uint32_t idx, data; };
struct FaultEvt { uint32_t vec; };

static void clear_inputs(Vpenumbra3_spine_test* dut) {
    dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
    dut->i_if_fault_pending = 0; dut->i_if_fault_vec = 0; dut->i_if_fault_status = 0;
    dut->i_fetch_busy = 0;
    dut->i_translate_paddr = 0; dut->i_translate_cacheable = 0; dut->i_translate_hit = 0;
    dut->i_translate_miss_fault = 0; dut->i_translate_prot_fault = 0;
    dut->i_dcache_hit = 0; dut->i_dcache_rdata = 0; dut->i_sys_rdata = 0;
    dut->i_fill_done = 0; dut->i_fill_data = 0; dut->i_fill_fault = 0;
    dut->i_irq_inject = 0; dut->i_irq_vec = 0;
}

// Reset, stream a program (no external memory), collect commits + faults.
static void run(Vpenumbra3_spine_test* dut, const std::vector<uint32_t>& prog,
                int max_cycles, std::vector<Commit>& commits,
                std::vector<FaultEvt>& faults) {
    clear_inputs(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    clear_inputs(dut);
    size_t pc = 0;
    for (int cyc = 0; cyc < max_cycles; cyc++) {
        bool have = pc < prog.size();
        dut->i_ir = have ? prog[pc] : 0;
        dut->i_valid = have ? 1 : 0;
        dut->i_pc = (uint32_t)(pc * 4);
        dut->i_next_pc = (uint32_t)(pc * 4 + 4);
        dut->eval();
        bool consume = dut->o_deq_ready;
        tick(dut);
        dut->eval();
        if (dut->o_commit_we) commits.push_back({dut->o_commit_idx, dut->o_commit_data});
        if (dut->o_fault_commit) faults.push_back({dut->o_fault_vec});
        if (have && consume) pc++;
    }
}

static void check_commits(const char* tag, const std::vector<Commit>& got,
                          const std::vector<Commit>& exp) {
    char n[40];
    snprintf(n, sizeof(n), "%s_count", tag);
    check(n, (uint32_t)got.size(), (uint32_t)exp.size());
    for (size_t i = 0; i < exp.size() && i < got.size(); i++) {
        snprintf(n, sizeof(n), "%s_%zu_idx", tag, i);
        check(n, got[i].idx, exp[i].idx);
        snprintf(n, sizeof(n), "%s_%zu_data", tag, i);
        check(n, got[i].data, exp[i].data);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_spine_test* dut = new Vpenumbra3_spine_test;

    // ── Reset sanity ─────────────────────────────────────────────
    clear_inputs(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_retire", dut->o_retire_valid, 0);
    check("rst_commit", dut->o_commit_we, 0);

    // ── Straight-line ALU + forwarding ───────────────────────────
    // LLI/ADDI chains (EX->EX leg 0) and a 2-ahead read (leg 1).
    {
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI,  1, 0x10),   // R1 = 0x10
            enc_l(OP_L_ADDI, 1, 0x01),   // R1 = 0x11  (fwd R1, 1-ahead)
            enc_l(OP_L_ADDI, 1, 0x01),   // R1 = 0x12  (fwd R1, 1-ahead)
            enc_l(OP_L_LLI,  2, 0x55),   // R2 = 0x55
            enc_r(OP_R_ADD,  1, 2, 0),   // R1 = R1+R2 = 0x67 (fwd R1 2-ahead, R2 1-ahead)
        };
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 64, got, flt);
        check_commits("alu", got, {{1,0x10},{1,0x11},{1,0x12},{2,0x55},{1,0x67}});
        check("alu_nofault", (uint32_t)flt.size(), 0);
    }

    // ── divmul dual-write (the wb_local_stall freeze) ────────────
    // MUL R1,R2 -> R1 (lo) and R3 (hi); both halves commit, in two cycles.
    {
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 1, 6),                       // R1 = 6
            enc_l(OP_L_LLI, 2, 7),                       // R2 = 7
            enc_r(OP_R_MUL, 1, 2, 0) | (3 << 12),        // R1=lo=42, R3=hi=0
        };
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 80, got, flt);
        check_commits("mul", got, {{1,6},{2,7},{1,42},{3,0}});
        check("mul_nofault", (uint32_t)flt.size(), 0);
    }

    // ── Precise fault: an illegal op faults, commits no register ─
    {
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 1, 0x11),    // R1 = 0x11 (commits)
            enc_r(OP_R_RESERVED, 1, 2, 0),  // reserved opcode -> VEC_ILLEGAL at WB
        };
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 48, got, flt);
        check_commits("flt", got, {{1,0x11}});       // only the LLI committed
        check("flt_count", (uint32_t)flt.size(), 1);
        if (!flt.empty()) check("flt_vec", flt[0].vec, VEC_ILLEGAL);
    }

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

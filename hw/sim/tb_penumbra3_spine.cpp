// Verilator testbench for penumbra3_spine (via penumbra3_spine_test).
//
// Drives the spine as a pipeline with a fetch-stream model: a program is
// presented at the FIFO head and advanced on o_deq_ready, while commits (the
// regfile write port) and fault-commits are collected. The collected sequences
// are checked against the expected streams -- exercising issue, operand
// forwarding, the scoreboard interlock, the divmul dual-write (the wb_local_stall
// freeze), precise faults, and in-order commit.
//
// Loads and stores run against a model of the translation and the uncore: the
// mapping is identity, the L1 never hits, and each request on the port answers
// a set number of cycles after it rises. A cacheable store goes to the store
// buffer instead, which always has room. The address decides the rest:
//   below 0x4000             cacheable
//   0x4000 and up            uncacheable
//   0x6000..0x6FFF           no device: the access completes with a bus fault
// A fault stops the program stream and starts the handler stream, as the
// vector fetch would, so a handler shows what state the fault left behind.

#include <cstdio>
#include <cstdint>
#include <vector>
#include <set>
#include <map>
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
static uint32_t enc_m(int L, int sz, int rd, int rb, uint16_t off) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27)
         | ((rd & 0xF) << 22) | ((rb & 0xF) << 18) | ((uint32_t)off << 2);
}
static uint32_t ldw(int rd, int rb, uint16_t off) { return enc_m(1, 2, rd, rb, off); }
static uint32_t stw(int rd, int rb, uint16_t off) { return enc_m(0, 2, rd, rb, off); }

enum { OP_L_LLI = 0, OP_L_ADDI = 3, OP_R_ADD = 0, OP_R_MUL = 16, OP_R_RESERVED = 12,
       OP_R_WRSYS = 23, OP_R_RDSYS = 24, OP_R_DI = 29 };
static uint32_t rdsys(int rd, int dev, int reg) {
    return enc_r(OP_R_RDSYS, rd, 0, 0) | ((dev & 0xF) << 12) | ((reg & 0xF) << 8);
}
static uint32_t wrsys(int rd, int dev, int reg) {
    return enc_r(OP_R_WRSYS, rd, 0, 0) | ((dev & 0xF) << 12) | ((reg & 0xF) << 8);
}
enum { VEC_BUS_FAULT = 0, VEC_ILLEGAL = 7, VEC_ALIGN = 8 };
enum { FAULT_BUS = 4, FSTAT_R = 1 << 8, FSTAT_W = 1 << 9 };
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_DI = 12 };   // op_class_e

struct Commit { uint32_t idx, data; };
struct FaultEvt { uint32_t vec; };

// Everything one run observes. The cycle lists are in run order.
struct Trace {
    std::vector<Commit>   commits;
    std::vector<FaultEvt> faults;
    std::vector<uint32_t> retire_pcs, retire_cycles, retire_ops, retire_srs;
    std::vector<uint32_t> dc_cycles, dc_pcs, dc_ops;
    std::set<uint32_t>    ex_pcs, mem_pcs;   // every PC seen in EX, in MEM2
    std::map<uint32_t, uint32_t> ex_arrival;  // PC -> the cycle it first reached EX
    std::vector<uint32_t> sys_wdata, sys_cycles;   // each sysreg write
    std::vector<uint32_t> done_cycles;
    uint32_t mmu_faults = 0, fault_vaddr = 0, fault_status = 0, epc = 0;
    uint32_t requests = 0;
    uint32_t enqueues = 0, enq_addr = 0, enq_wdata = 0;   // stores into the buffer
    uint32_t retire_with_dc = 0;   // cycles a retire and a drain-commit share
};

static bool cacheable(uint32_t a) { return a < 0x4000; }
static bool no_device(uint32_t a) { return (a & 0xF000) == 0x6000; }
static uint32_t mem_word(uint32_t a) { return ((a >> 2) * 2654435761u) ^ 0x5A5AA5A5u; }

static void clear_inputs(Vpenumbra3_spine_test* dut) {
    dut->i_ir = 0; dut->i_pc = 0; dut->i_next_pc = 0; dut->i_valid = 0;
    dut->i_if_fault_pending = 0; dut->i_if_fault_vec = 0; dut->i_if_fault_status = 0;
    dut->i_fetch_busy = 0;
    dut->i_translate_paddr = 0; dut->i_translate_cacheable = 0; dut->i_translate_hit = 0;
    dut->i_translate_miss_fault = 0; dut->i_translate_prot_fault = 0;
    dut->i_sys_rdata = 0;
    dut->i_drsp_done = 0; dut->i_rsp_fault = 0;
    for (int k = 0; k < 4; k++) dut->i_rsp_line[k] = 0;
    dut->i_store_full = 0;
    dut->i_irq_inject = 0; dut->i_irq_vec = 0;
    dut->i_deferred_inject = 0; dut->i_deferred_addr = 0;
}

// Reset and stream a program, then its handler once a fault commits. Each
// request on the port answers latency cycles after it rises.
static void run_mem(Vpenumbra3_spine_test* dut, const std::vector<uint32_t>& prog,
                    const std::vector<uint32_t>& handler, int max_cycles, Trace& t,
                    int latency = 3) {
    clear_inputs(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    clear_inputs(dut);
    const std::vector<uint32_t>* stream = &prog;
    uint32_t base = 0;
    size_t pc = 0;
    int waited = 0;
    int blank = 0;                       // cycles the redirect leaves fetch empty
    bool req_prev = false;
    for (int cyc = 0; cyc < max_cycles; cyc++) {
        bool have = blank == 0 && pc < stream->size();
        dut->i_ir = have ? (*stream)[pc] : 0;
        dut->i_valid = have ? 1 : 0;
        dut->i_pc = base + (uint32_t)(pc * 4);
        dut->i_next_pc = base + (uint32_t)(pc * 4 + 4);
        dut->eval();
        bool consume = dut->o_deq_ready;
        bool launch_en = dut->o_translate_en;
        uint32_t launch_va = dut->o_translate_vaddr;
        bool launch_hold = dut->o_mem_launch_hold;
        tick(dut);
        dut->eval();

        // The verdict for the launch that just left MEM1; it stands while
        // the launch side is held.
        if (!launch_hold) {
            dut->i_translate_paddr = launch_va;
            dut->i_translate_cacheable = cacheable(launch_va);
            dut->i_translate_hit = launch_en;
        }

        // The uncore
        bool req = dut->o_dreq_valid;
        if (req && !req_prev) t.requests++;
        req_prev = req;
        dut->i_drsp_done = 0;
        dut->i_rsp_fault = 0;
        if (req && ++waited == latency) {
            uint32_t a = dut->o_dreq_addr;
            waited = 0;
            dut->i_drsp_done = 1;
            dut->i_rsp_fault = no_device(a);
            for (int k = 0; k < 4; k++) {
                if (cacheable(a)) dut->i_rsp_line[k] = mem_word((a & ~0xFu) + 4 * k);
                else              dut->i_rsp_line[k] = k ? 0 : mem_word(a);
            }
            t.done_cycles.push_back(cyc);
        }
        dut->eval();

        if (dut->o_store_enq) {
            t.enqueues++;
            t.enq_addr = dut->o_store_addr;
            t.enq_wdata = dut->o_dcache_wdata;
        }
        if (dut->o_commit_we)
            t.commits.push_back({dut->o_commit_idx, dut->o_commit_data});
        if (dut->o_insn_committed) {
            t.retire_pcs.push_back(dut->o_retire_pc);
            t.retire_cycles.push_back(cyc);
            t.retire_ops.push_back(dut->o_retire_op_class);
            t.retire_srs.push_back(dut->o_retire_sr);
        }
        if (dut->o_dc_commit) {
            t.dc_cycles.push_back(cyc);
            t.dc_pcs.push_back(dut->o_dc_commit_pc);
            t.dc_ops.push_back(dut->o_dc_commit_op_class);
        }
        if (dut->o_ex_valid)  t.ex_pcs.insert(dut->o_ex_pc);
        if (dut->o_ex_valid && !t.ex_arrival.count(dut->o_ex_pc))
            t.ex_arrival[dut->o_ex_pc] = cyc;
        if (dut->o_sys_we) {
            t.sys_wdata.push_back(dut->o_sys_wdata);
            t.sys_cycles.push_back(cyc);
        }
        if (dut->o_mem_valid) t.mem_pcs.insert(dut->o_mem_pc);
        if (dut->o_insn_committed && dut->o_dc_commit) t.retire_with_dc++;
        if (dut->o_mmu_fault_commit) {
            t.mmu_faults++;
            t.fault_vaddr = dut->o_mmu_fault_vaddr;
            t.fault_status = dut->o_mmu_fault_status;
        }
        if (have && consume) pc++;
        if (blank) blank--;
        if (dut->o_fault_commit) {
            t.faults.push_back({dut->o_fault_vec});
            stream = &handler;
            base = 0x100;
            pc = 0;
            blank = 2;
        }
    }
    t.epc = dut->o_epc;
}

// A program with no handler, for the runs that only compare commits and faults.
static void run(Vpenumbra3_spine_test* dut, const std::vector<uint32_t>& prog,
                int max_cycles, std::vector<Commit>& commits,
                std::vector<FaultEvt>& faults) {
    Trace t;
    run_mem(dut, prog, {}, max_cycles, t);
    commits = t.commits;
    faults = t.faults;
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

static void check_retires(const char* tag, const Trace& t,
                          const std::vector<uint32_t>& exp) {
    char n[40];
    snprintf(n, sizeof(n), "%s_retire_count", tag);
    check(n, (uint32_t)t.retire_pcs.size(), (uint32_t)exp.size());
    for (size_t i = 0; i < exp.size() && i < t.retire_pcs.size(); i++) {
        snprintf(n, sizeof(n), "%s_retire_%zu_pc", tag, i);
        check(n, t.retire_pcs[i], exp[i]);
    }
}

// The trace outputs: each retirement's op class, and every retired PC seen
// in EX and in MEM2 on its way.
static void check_trace(const char* tag, const Trace& t,
                        const std::vector<uint32_t>& ops) {
    char n[48];
    for (size_t i = 0; i < ops.size() && i < t.retire_ops.size(); i++) {
        snprintf(n, sizeof(n), "%s_retire_%zu_op", tag, i);
        check(n, t.retire_ops[i], ops[i]);
    }
    for (size_t i = 0; i < t.retire_pcs.size(); i++) {
        snprintf(n, sizeof(n), "%s_retire_%zu_in_ex", tag, i);
        check(n, t.ex_pcs.count(t.retire_pcs[i]), 1);
        snprintf(n, sizeof(n), "%s_retire_%zu_in_mem", tag, i);
        check(n, t.mem_pcs.count(t.retire_pcs[i]), 1);
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

    // ── Every RAW distance from one ALU producer ─────────────────
    // At full rate the consumer d instructions behind reads R1 from EX/MEM1
    // (d=1), MEM1/MEM2 (2), MEM2/WB (3), the regfile in the cycle the
    // producer writes it (4: the write-first read, nothing left to forward
    // once it reaches EX), and the regfile after the write (5).
    {
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 2, 0), enc_l(OP_L_LLI, 3, 0), enc_l(OP_L_LLI, 4, 0),
            enc_l(OP_L_LLI, 5, 0), enc_l(OP_L_LLI, 6, 0),
            enc_l(OP_L_LLI, 1, 0x1234),  // the producer
            enc_r(OP_R_ADD, 2, 1, 0),    // d=1
            enc_r(OP_R_ADD, 3, 1, 0),    // d=2
            enc_r(OP_R_ADD, 4, 1, 0),    // d=3
            enc_r(OP_R_ADD, 5, 1, 0),    // d=4
            enc_r(OP_R_ADD, 6, 1, 0),    // d=5
        };
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 64, got, flt);
        check_commits("raw", got, {{2,0},{3,0},{4,0},{5,0},{6,0},{1,0x1234},
                                   {2,0x1234},{3,0x1234},{4,0x1234},{5,0x1234},{6,0x1234}});
    }

    // ── A WRSYS's value at every distance from its producer ──────
    // The WRSYS takes its Rd as it reaches EX, from the leg its producer is
    // on (d=1..3) or the regfile (4, 5), and writes it once the back end has
    // drained, by when the producer has left every leg. The regfile keeps its
    // contents across runs, so each run's value is its own.
    for (int d = 1; d <= 5; d++) {
        const uint16_t v = (uint16_t)(0x4D00 | d);
        std::vector<uint32_t> prog = {enc_l(OP_L_LLI, 1, v)};
        for (int k = 1; k < d; k++) prog.push_back(enc_l(OP_L_LLI, 2, (uint16_t)k));
        prog.push_back(wrsys(1, 0, 5));
        Trace t;
        run_mem(dut, prog, {}, 60, t);
        char tag[32];
        snprintf(tag, sizeof tag, "wrsys_d%d_writes", d);
        check(tag, (uint32_t)t.sys_wdata.size(), 1);
        snprintf(tag, sizeof tag, "wrsys_d%d_value", d);
        check(tag, t.sys_wdata.empty() ? 0 : t.sys_wdata[0], v);
    }

    // Behind a DI, which drains the back end, the WRSYS reaches EX with
    // nothing in flight and commits in that first cycle, with the value read
    // at issue.
    {
        Trace t;
        run_mem(dut,
                {enc_l(OP_L_LLI, 1, 0x5678), enc_r(OP_R_DI, 0, 0, 0), wrsys(1, 0, 5)},
                {}, 60, t);
        check("wrsys_drained_writes", (uint32_t)t.sys_wdata.size(), 1);
        check("wrsys_drained_value", t.sys_wdata.empty() ? 0 : t.sys_wdata[0], 0x5678);
        check("wrsys_drained_first_cycle",
              !t.sys_cycles.empty() && t.ex_arrival.count(8)
                  && t.sys_cycles[0] == t.ex_arrival[8], 1);
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

    // The hi half read behind the MUL, at each distance: no forward leg
    // carries it, so the reader waits for its write. R3 holds another value
    // until then, a different one in each run (the regfile is not reset).
    for (int lead = 0; lead <= 3; lead++) {
        uint16_t stale = (uint16_t)(0x3300 | lead);
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 1, 6), enc_l(OP_L_LLI, 2, 7),
            enc_l(OP_L_LLI, 3, stale), enc_l(OP_L_LLI, 4, 0x100),
            enc_r(OP_R_MUL, 1, 2, 0) | (3 << 12),        // R1 = 42, R3 = 0
        };
        std::vector<Commit> want = {{1, 6}, {2, 7}, {3, stale}, {4, 0x100},
                                    {1, 42}, {3, 0}};
        for (int k = 0; k < lead; k++) {
            prog.push_back(enc_l(OP_L_LLI, 7, (uint16_t)k));
            want.push_back({7, (uint32_t)k});
        }
        prog.push_back(enc_r(OP_R_ADD, 4, 3, 0));        // R4 += R3
        want.push_back({4, 0x100});
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 120, got, flt);
        char tag[40];
        snprintf(tag, sizeof tag, "mul_hi_reader_%d", lead);
        check_commits(tag, got, want);
    }

    // The lo half read behind the MUL, at each distance: it forwards from
    // EX/MEM1 on, as an ALU result does, so its reader reaches EX in the
    // cycle an independent instruction in its place would.
    for (int d = 1; d <= 4; d++) {
        uint32_t gap[2] = {0, 0};
        for (int dep = 0; dep <= 1; dep++) {
            std::vector<uint32_t> prog = {
                enc_l(OP_L_LLI, 1, 6), enc_l(OP_L_LLI, 2, 7),
                enc_l(OP_L_LLI, 3, 0), enc_l(OP_L_LLI, 8, 0),
            };
            uint32_t mul_pc = (uint32_t)prog.size() * 4;
            prog.push_back(enc_r(OP_R_MUL, 1, 2, 0));            // R1 = 42
            for (int k = 1; k < d; k++) prog.push_back(enc_l(OP_L_LLI, 7, (uint16_t)k));
            uint32_t reader_pc = (uint32_t)prog.size() * 4;
            prog.push_back(enc_r(OP_R_ADD, 3, dep ? 1 : 8, 0));  // R3 += R1 or R8
            Trace t;
            run_mem(dut, prog, {}, 120, t);
            gap[dep] = t.ex_arrival.count(mul_pc) && t.ex_arrival.count(reader_pc)
                     ? t.ex_arrival[reader_pc] - t.ex_arrival[mul_pc] : 0;
            if (dep) {
                char tag[40];
                snprintf(tag, sizeof tag, "mul_lo_reader_d%d_value", d);
                check(tag, t.commits.empty() ? 0 : t.commits.back().data, 42);
            }
        }
        char tag[40];
        snprintf(tag, sizeof tag, "mul_lo_reader_d%d_gap", d);
        check(tag, gap[1] != 0 && gap[1] == gap[0], 1);
    }

    // With Rdh = Rd the register ends up holding the hi half, which no leg
    // carries: a reader directly behind waits for the second write rather
    // than take the lo half forwarded. 0x10000 squared: lo 0, hi 1.
    {
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 3, 0), enc_l(OP_L_LLI, 1, 0x8000),
            enc_r(OP_R_ADD, 1, 1, 0),                    // R1 = 0x10000
            enc_r(OP_R_MUL, 1, 1, 0) | (1 << 12),        // R1 = lo 0, then hi 1
            enc_r(OP_R_ADD, 3, 1, 0),                    // R3 += R1
        };
        std::vector<Commit> got; std::vector<FaultEvt> flt;
        run(dut, prog, 120, got, flt);
        check_commits("mul_rdh_is_rd", got,
                      {{3, 0}, {1, 0x8000}, {1, 0x10000}, {1, 0}, {1, 1}, {3, 1}});
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

    // ── A drain-commit waits for the instruction ahead of it ─────
    // Nothing older than the LLI, so it crosses MEM2 with both EX/MEM1 and
    // MEM2/WB empty. The DI behind it must still commit after it retires.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 1, 0x11), enc_r(OP_R_DI, 0, 0, 0)}, {}, 30, t);
        check_retires("dc_alu", t, {0});
        check("dc_alu_commits", (uint32_t)t.dc_cycles.size(), 1);
        check("dc_alu_dc_pc", t.dc_pcs.empty() ? 0xFF : t.dc_pcs[0], 4);
        check("dc_alu_dc_op", t.dc_ops.empty() ? 0xFF : t.dc_ops[0], OPC_DI);
        if (!t.dc_cycles.empty() && !t.retire_cycles.empty())
            check("dc_alu_after_retire", t.dc_cycles[0] > t.retire_cycles[0], 1);
    }

    // ── A fault releases what the flush squashed ─────────────────
    // The LDW R7 behind the misaligned load issues and sets R7's pending
    // bit. The fault squashes it, so nothing will ever write R7, and the
    // handler reads R7.
    {
        Trace t;
        run_mem(dut,
                {enc_l(OP_L_LLI, 7, 0x33),
                 enc_l(OP_L_LLI, 5, 0x1001),       // R5 = a misaligned address
                 ldw(6, 5, 0),                     // align fault at WB
                 ldw(7, 0, 0x1000)},               // squashed
                {enc_l(OP_L_ADDI, 7, 1)},          // R7 = R7 + 1
                60, t);
        check("flush_faults", (uint32_t)t.faults.size(), 1);
        if (!t.faults.empty()) check("flush_vec", t.faults[0].vec, VEC_ALIGN);
        check("flush_no_request", t.requests, 0);
        check_commits("flush", t.commits, {{7, 0x33}, {5, 0x1001}, {7, 0x34}});
    }

    // ── A parked load retires as it completes ────────────────────
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), ldw(6, 5, 8), enc_l(OP_L_ADDI, 6, 1)},
                {}, 40, t);
        uint32_t w = mem_word(0x1008);
        check_commits("parked_load", t.commits, {{5, 0x1000}, {6, w}, {6, w + 1}});
        check_retires("parked_load", t, {0, 4, 8});
        check_trace("parked_load", t, {OPC_ALU, OPC_LOAD, OPC_ALU});
        check("parked_load_sr_s", t.retire_srs.empty() ? 0 : t.retire_srs[0] >> 31, 1);
        check("parked_load_requests", t.requests, 1);
        if (t.retire_cycles.size() > 1 && !t.done_cycles.empty())
            check("parked_load_retires_after_done",
                  t.retire_cycles[1] > t.done_cycles[0], 1);
    }

    // A parked load whose destination is its own base, used at once.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 1, 0x1000), ldw(1, 1, 8), enc_l(OP_L_ADDI, 1, 1)},
                {}, 40, t);
        uint32_t w = mem_word(0x1008);
        check_commits("parked_load_self", t.commits, {{1, 0x1000}, {1, w}, {1, w + 1}});
    }

    // An older late writer of the parked load's destination (an RDSYS,
    // committing at WB as the load parks): its write must not release the
    // load's readers. The fillers set where the RDSYS's commit falls.
    for (int lead = 0; lead <= 2; lead++) {
        Trace t;
        std::vector<uint32_t> prog = {enc_l(OP_L_LLI, 5, 0x1000), rdsys(1, 8, 0)};
        for (int k = 0; k < lead; k++) prog.push_back(enc_l(OP_L_LLI, 7, (uint16_t)k));
        prog.push_back(ldw(1, 5, 8));
        prog.push_back(enc_l(OP_L_ADDI, 1, 1));
        run_mem(dut, prog, {}, 60, t);
        uint32_t w = mem_word(0x1008);
        char tag[40];
        snprintf(tag, sizeof tag, "older_late_writer_%d", lead);
        check(tag, t.commits.empty() ? 0 : t.commits.back().data, w + 1);
    }

    // Two loads to one register: the first's completion must not release
    // the second's readers.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), ldw(1, 5, 8), ldw(1, 5, 0x40),
                      enc_l(OP_L_ADDI, 1, 1)}, {}, 80, t);
        uint32_t w = mem_word(0x1040);
        check("two_loads_one_reg", t.commits.empty() ? 0 : t.commits.back().data, w + 1);
    }

    // The jump-table shape: an ALU result as the base, the loaded word added
    // back to the same base register.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 10, 0x1000), enc_l(OP_L_LLI, 1, 4),
                      enc_r(OP_R_ADD, 1, 10, 0), ldw(1, 1, 4), enc_r(OP_R_ADD, 1, 10, 0)},
                {}, 50, t);
        uint32_t w = mem_word(0x1008);
        check_commits("parked_load_table", t.commits,
                      {{10, 0x1000}, {1, 4}, {1, 0x1004}, {1, w}, {1, w + 0x1000}});
    }

    // ── A load's readers are released as it reaches MEM2 ─────────
    // With the L1-D enabled a load that hits forwards from MEM2/WB, so its
    // reader d behind it reaches EX 3 cycles after the load for d up to 3,
    // and d cycles after it from there. The first load fills the line; the
    // fillers keep the freeze it takes clear of the timed load.
    for (int d = 1; d <= 4; d++) {
        Trace t;
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 2, 1), wrsys(2, 2, 1),   // L1-D CTRL.ENABLE
            enc_l(OP_L_LLI, 5, 0x1000), enc_l(OP_L_LLI, 3, 0),
            ldw(6, 5, 0),                            // the miss, filling the line
        };
        for (int k = 0; k < 4; k++) prog.push_back(enc_l(OP_L_LLI, 7, (uint16_t)k));
        uint32_t load_pc = (uint32_t)prog.size() * 4;
        prog.push_back(ldw(1, 5, 4));                // the timed load, a hit
        for (int k = 1; k < d; k++) prog.push_back(enc_l(OP_L_LLI, 7, (uint16_t)k));
        uint32_t reader_pc = (uint32_t)prog.size() * 4;
        prog.push_back(enc_r(OP_R_ADD, 3, 1, 0));
        run_mem(dut, prog, {}, 120, t);
        char tag[40];
        snprintf(tag, sizeof tag, "release_hit_d%d_requests", d);
        check(tag, t.requests, 1);                   // the timed load hit
        snprintf(tag, sizeof tag, "release_hit_d%d_value", d);
        check(tag, t.commits.empty() ? 0 : t.commits.back().data, mem_word(0x1004));
        snprintf(tag, sizeof tag, "release_hit_d%d_gap", d);
        check(tag, t.ex_arrival.count(load_pc) && t.ex_arrival.count(reader_pc)
                   ? t.ex_arrival[reader_pc] - t.ex_arrival[load_pc] : 0,
              d <= 3 ? 3 : (uint32_t)d);
    }

    // A sysreg read is released the same way, and never parks.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 3, 0), rdsys(1, 8, 0), enc_r(OP_R_ADD, 3, 1, 0)},
                {}, 40, t);
        check("release_rdsys_gap",
              t.ex_arrival.count(4) && t.ex_arrival.count(8)
                  ? t.ex_arrival[8] - t.ex_arrival[4] : 0, 3);
    }

    // A reader released behind a load that parks waits in EX for the load's
    // word: it is there before the done, and takes the word from MEM2/WB.
    // The completion enters MEM2/WB in the L1-D's done, a cycle after the
    // uncore's, and retires at WB a cycle later; the reader leaves EX as
    // the load retires, three stages behind it.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), enc_l(OP_L_LLI, 3, 0),
                      enc_l(OP_L_LLI, 6, 0x6601),        // what a stale read gives
                      ldw(6, 5, 8), enc_r(OP_R_ADD, 3, 6, 0)}, {}, 40, t);
        check("release_miss_value", t.commits.empty() ? 0 : t.commits.back().data,
              mem_word(0x1008));
        check("release_miss_in_ex_before_done",
              t.ex_arrival.count(16) && !t.done_cycles.empty()
                  && t.ex_arrival[16] < t.done_cycles[0], 1);
        auto retired = [&](uint32_t pc) -> uint32_t {
            for (size_t k = 0; k < t.retire_pcs.size(); k++)
                if (t.retire_pcs[k] == pc) return t.retire_cycles[k];
            return 0;
        };
        uint32_t r = t.done_cycles.empty() ? 0 : t.done_cycles[0];
        check("release_miss_load_retires",   retired(12) - r, 2);
        check("release_miss_reader_retires", retired(16) - r, 5);
    }

    // The same with the launch side resuming a cycle after the completion:
    // the load held in MEM2 behind the miss has its lookup in the set the
    // line goes into, so the forward stays on the leg a cycle more.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 2, 1), wrsys(2, 2, 1),
                      enc_l(OP_L_LLI, 5, 0x1000), enc_l(OP_L_LLI, 8, 0),
                      enc_l(OP_L_LLI, 6, 0x6602),
                      ldw(6, 5, 8), ldw(7, 5, 12), enc_r(OP_R_ADD, 8, 6, 0)},
                {}, 60, t);
        check("release_held_requests", t.requests, 1);
        check_commits("release_held", t.commits,
                      {{2, 1}, {5, 0x1000}, {8, 0}, {6, 0x6602}, {6, mem_word(0x1008)},
                       {7, mem_word(0x100C)}, {8, mem_word(0x1008)}});
    }

    // A divmul and a WRSYS released behind a parked load take the word too:
    // neither launches nor takes its value before the completion.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), enc_l(OP_L_LLI, 1, 3),
                      enc_l(OP_L_LLI, 6, 0x6603),
                      ldw(6, 5, 8), enc_r(OP_R_MUL, 1, 6, 0) | (3 << 12)}, {}, 80, t);
        uint64_t p = 3ull * mem_word(0x1008);       // R1 = lo, R3 = hi
        check_commits("release_mul", t.commits,
                      {{5, 0x1000}, {1, 3}, {6, 0x6603}, {6, mem_word(0x1008)},
                       {1, (uint32_t)p}, {3, (uint32_t)(p >> 32)}});
    }
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), enc_l(OP_L_LLI, 6, 0x6604),
                      ldw(6, 5, 8), wrsys(6, 7, 0)}, {}, 60, t);
        check("release_wrsys_count", (uint32_t)t.sys_wdata.size(), 1);
        check("release_wrsys_value", t.sys_wdata.empty() ? 0 : t.sys_wdata[0],
              mem_word(0x1008));
    }

    // ── A cacheable store retires once, into the store buffer ────
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), stw(5, 5, 4), enc_l(OP_L_LLI, 7, 7)},
                {}, 40, t);
        check_commits("c_store", t.commits, {{5, 0x1000}, {7, 7}});
        check_retires("c_store", t, {0, 4, 8});
        check_trace("c_store", t, {OPC_ALU, OPC_STORE, OPC_ALU});
        check("c_store_requests", t.requests, 0);
        check("c_store_enqueues", t.enqueues, 1);
        check("c_store_enq_addr", t.enq_addr, 0x1004);
        check("c_store_enq_data", t.enq_wdata, 0x1000);
    }

    // ── An uncached store retires with its completion ────────────
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x4000), stw(5, 5, 4), enc_l(OP_L_LLI, 7, 7)},
                {}, 40, t);
        check_commits("uc_store", t.commits, {{5, 0x4000}, {7, 7}});
        check_retires("uc_store", t, {0, 4, 8});
        check_trace("uc_store", t, {OPC_ALU, OPC_STORE, OPC_ALU});
        check("uc_store_requests", t.requests, 1);
        if (t.retire_cycles.size() > 1 && !t.done_cycles.empty())
            check("uc_store_retires_after_done",
                  t.retire_cycles[1] > t.done_cycles[0], 1);
    }

    // ── A load's bus fault is precise ────────────────────────────
    // The fault squashes the two LLIs behind the load and writes nothing.
    {
        Trace t;
        run_mem(dut,
                {enc_l(OP_L_LLI, 6, 0x66), enc_l(OP_L_LLI, 5, 0x6000), ldw(6, 5, 8),
                 enc_l(OP_L_LLI, 7, 0x77), enc_l(OP_L_LLI, 8, 0x88)},
                {enc_l(OP_L_ADDI, 6, 1)},          // R6 was left alone and is free
                60, t);
        check_commits("bf_load", t.commits, {{6, 0x66}, {5, 0x6000}, {6, 0x67}});
        check("bf_load_faults", (uint32_t)t.faults.size(), 1);
        if (!t.faults.empty()) check("bf_load_vec", t.faults[0].vec, VEC_BUS_FAULT);
        check("bf_load_epc", t.epc, 8);
        check("bf_load_mmu_commit", t.mmu_faults, 1);
        check("bf_load_vaddr", t.fault_vaddr, 0x6008);
        check("bf_load_status", t.fault_status, FAULT_BUS | FSTAT_R);
        check_retires("bf_load", t, {0, 4, 8, 0x100});
    }

    // ── An uncached store's bus fault is precise ─────────────────
    {
        Trace t;
        run_mem(dut,
                {enc_l(OP_L_LLI, 5, 0x6000), stw(5, 5, 4), enc_l(OP_L_LLI, 7, 0x77)},
                {}, 50, t);
        check_commits("bf_store", t.commits, {{5, 0x6000}});
        check("bf_store_faults", (uint32_t)t.faults.size(), 1);
        if (!t.faults.empty()) check("bf_store_vec", t.faults[0].vec, VEC_BUS_FAULT);
        check("bf_store_epc", t.epc, 4);
        check("bf_store_vaddr", t.fault_vaddr, 0x6004);
        check("bf_store_status", t.fault_status, FAULT_BUS | FSTAT_W);
        check_retires("bf_store", t, {0, 4});
    }

    // ── A bus fault releases what the flush squashed ─────────────
    {
        Trace t;
        run_mem(dut,
                {enc_l(OP_L_LLI, 7, 0x33), enc_l(OP_L_LLI, 5, 0x6000), ldw(6, 5, 0),
                 ldw(7, 0, 0x1000)},
                {enc_l(OP_L_ADDI, 7, 1)},
                60, t);
        check("bf_flush_faults", (uint32_t)t.faults.size(), 1);
        check("bf_flush_one_request", t.requests, 1);   // the squashed load asks nothing
        check_commits("bf_flush", t.commits, {{7, 0x33}, {5, 0x6000}, {7, 0x34}});
    }

    // ── A drain-commit waits for a parked access ─────────────────
    // The DI sits in EX with EX/MEM1, MEM1/MEM2 and MEM2/WB all empty, and
    // the access may yet fault.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), ldw(6, 5, 0), enc_r(OP_R_DI, 0, 0, 0)},
                {}, 40, t, 8);
        check_retires("dc_load", t, {0, 4});
        check("dc_load_commits", (uint32_t)t.dc_cycles.size(), 1);
        if (!t.dc_cycles.empty() && !t.done_cycles.empty())
            check("dc_load_after_done", t.dc_cycles[0] > t.done_cycles[0], 1);
        check("dc_load_apart", t.retire_with_dc, 0);    // each retire counted once
    }
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x4000), stw(5, 5, 0), enc_r(OP_R_DI, 0, 0, 0)},
                {}, 40, t, 8);
        check("dc_store_commits", (uint32_t)t.dc_cycles.size(), 1);
        if (!t.dc_cycles.empty() && !t.done_cycles.empty())
            check("dc_store_after_done", t.dc_cycles[0] > t.done_cycles[0], 1);
    }
    // A cacheable store parks nothing: the drain-commit behind it waits for
    // no write, which the store buffer orders on its own.
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x1000), stw(5, 5, 0), enc_r(OP_R_DI, 0, 0, 0)},
                {}, 40, t, 8);
        check("dc_cstore_commits", (uint32_t)t.dc_cycles.size(), 1);
        check("dc_cstore_no_request", t.requests, 0);
        check("dc_cstore_enqueued", t.enqueues, 1);
    }

    // ── A faulting access takes the drain-commit behind it with it ──
    {
        Trace t;
        run_mem(dut, {enc_l(OP_L_LLI, 5, 0x6000), ldw(6, 5, 0), enc_r(OP_R_DI, 0, 0, 0)},
                {}, 40, t, 8);
        check("dc_flush_faults", (uint32_t)t.faults.size(), 1);
        check("dc_flush_no_commit", (uint32_t)t.dc_cycles.size(), 0);
    }

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

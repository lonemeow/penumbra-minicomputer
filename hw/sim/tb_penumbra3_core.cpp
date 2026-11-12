// Verilator testbench for penumbra3_core (via penumbra3_core_test).
//
// Exercises the whole core integration -- IF1/IF2 + fetch replay + elastic
// fetch buffer + spine + vecfetch + irq -- through a real L1-I and a model of the
// uncore's fetch side: each request answers a set number of cycles after it
// rises, with the line, or the word in word 0, from a ROM. A read of an
// address in the 0xFFFE0000 page finds no device and faults. The programs are
// ALU + branch only, so the D-side is tied off. What the L1 kept shows as the
// line reads that later fetches do not make.
//
// Each run sets the page verdict and the L1's enable, so the same programs run
// as an uncached machine does (the boot ROM's: every fetch a word read), as a
// cached one does, and with the L1 off on a cacheable page.
//
// Coverage:
//   - streaming + in-order commit, a replay at every miss
//   - branch redirect: wrong-path instructions never commit, and a read the
//     redirect left in flight neither reaches the target nor is lost to the L1
//   - interrupt entry: the vector fetch reads the handler address as a word
//     read, and the handler runs
//   - a fault with a read in flight: the vector read waits for it
//   - a fetch bus fault, on a word read and on a line read
//   - a deferred bus fault from the L1-D's store fault: taken with SR.I
//     clear, at the next boundary (EPC the first instruction not committed),
//     with the write's physical address and its own type in the MMU's fault
//     registers
//   - each line read once; the L1's counters seeing one miss per read
//   - a slot held for FIFO room, its L1 lookup standing
//   - a counted loop: its BNE mispredicted while its counter trains and at
//     the exit, predicted taken between, also in a body short enough that
//     the BNE's read meets its own training write; with the L1 off nothing
//     is predicted and EX redirects behind every taken BNE
//   - calls and returns predicted from the word and the return stack, never
//     mispredicted
//   - a BNE predicted taken while it waits in IF2 for FIFO room, not
//     discarded
//
// ERET and WRSYS-resync redirects share the same composition arm as the branch
// and vector-fetch cases and are exercised at the machine + program-suite level
// (the spine unit test already covers their commit-side behavior).

#include <cstdio>
#include <cstdint>
#include <vector>
#include <map>
#include "Vpenumbra3_core_test.h"
#include "verilated.h"

typedef Vpenumbra3_core_test Dut;

// ── ISA constants (penumbra_pkg / penumbra3_pkg) ─────────────────
enum { OP_R_ADD = 0, OP_R_RESERVED = 12, OP_R_MUL = 16, OP_R_EI = 28, OP_R_RDSPR = 31 };
enum { SPR_EPC = 1 };
enum { OPC_ALU = 0, OPC_EI = 11 };   // op_class_e
enum { OP_L_LLI = 0, OP_L_ADDI = 3, OP_L_SUBI = 4, OP_L_JMP = 11 };
enum { COND_AL = 0, COND_NE = 2, COND_BL = 15 };
enum { REG_LR = 13 };
enum { VEC_BUS_FAULT = 0, VEC_ILLEGAL = 7, VEC_EXT_IRQ = 9 };
enum { FAULT_BUS = 4, FAULT_DEFERRED_BUS = 5, FSTAT_W = 1 << 9, FSTAT_X = 1 << 10 };
// Cache device registers
enum { CACHE_CTRL = 1, CACHE_READ_HITS = 10, CACHE_READ_MISSES = 11 };

static const uint32_t RESET_PC = 0xFFFF0000u;

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

// ── Instruction encoders (doc/system/instruction-encoding.md) ────
static uint32_t enc_l(int op, int rd, uint16_t imm) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22) | imm;
}
static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
}
static uint32_t enc_b(int cond, uint32_t off22) {
    return (3u << 30) | ((cond & 0xF) << 26) | ((off22 & 0x3FFFFF) << 4);
}
// A branch at `at` targeting `tgt`: imm = sext(off22)<<2, target = PC + imm.
static uint32_t enc_b_to(uint32_t at, uint32_t tgt, int cond) {
    int32_t off = ((int32_t)(tgt - at)) >> 2;
    return enc_b(cond, (uint32_t)off);
}

// ── The ROM behind the uncore ────────────────────────────────────
// Unfilled addresses read a branch-to-self (target = PC + 0): the PC parks
// there committing nothing, so a program halts simply by running off its end.
static const uint32_t ROM_FILLER = (3u << 30);   // enc_b(COND_AL, 0)
static std::map<uint32_t, uint32_t> g_rom;

static uint32_t rom_at(uint32_t a) {
    auto it = g_rom.find(a);
    return it != g_rom.end() ? it->second : ROM_FILLER;
}
static void load(uint32_t base, const std::vector<uint32_t>& prog) {
    for (size_t i = 0; i < prog.size(); i++) g_rom[base + i * 4] = prog[i];
}
static bool no_device(uint32_t a) { return (a >> 16) == 0xFFFE; }

// ── One run's setup and what it observed ─────────────────────────
struct Config {
    bool cacheable = false;   // the page verdict for every fetch
    bool l1_on = false;
    int  latency = 3;         // cycles from a request rising to its done
    int  irq_cycle = -1;
    int  store_fault_cycle = -1;   // the L1-D's store fault, for one cycle
    uint32_t store_fault_addr = 0;
};
struct Req { uint32_t addr; int cacheable; };
struct Commit { uint32_t idx, data; };
struct Branch { uint32_t pc, target; };
struct Trace {
    std::vector<Commit>   commits;
    std::vector<uint32_t> fault_vecs;
    std::vector<Req>      reqs;
    std::vector<Branch>   branches;                  // each taken branch
    std::vector<uint32_t> retire_ops, retire_srs;    // each retirement's
    std::vector<uint32_t> dc_pcs, dc_ops;            // each drain-commit's
    std::vector<uint32_t> predicted;      // each slot fetch was steered behind by its prediction
    std::vector<uint32_t> mispredicted;   // each transfer EX redirected fetch behind
    uint32_t mmu_faults = 0, fault_vaddr = 0, fault_status = 0;
};

static Config g_cfg;
static Trace* g_t;
static int    g_waited;
static bool   g_req_prev;

// The uncore, for the cycle a clock edge has just begun.
static void uncore(Dut* dut) {
    bool req = dut->o_ireq_valid;
    if (req && !g_req_prev && g_t)
        g_t->reqs.push_back({dut->o_ireq_addr, dut->o_ireq_cacheable});
    g_req_prev = req;
    dut->i_irsp_done = 0;
    dut->i_rsp_fault = 0;
    if (req && ++g_waited == g_cfg.latency) {
        uint32_t a = dut->o_ireq_addr;
        g_waited = 0;
        dut->i_irsp_done = 1;
        dut->i_rsp_fault = no_device(a);
        for (int k = 0; k < 4; k++) {
            if (dut->o_ireq_cacheable) dut->i_rsp_line[k] = rom_at((a & ~0xFu) + 4 * k);
            else                       dut->i_rsp_line[k] = k ? 0xBAD00000u : rom_at(a);
        }
    }
    dut->eval();
}

static void tick(Dut* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
    uncore(dut);
}

static void l1_write(Dut* dut, int reg, uint32_t value) {
    dut->i_l1_sys_reg = reg; dut->i_l1_sys_wdata = value; dut->i_l1_sys_we = 1;
    tick(dut);
    dut->i_l1_sys_we = 0; dut->eval();
}
static uint32_t l1_read(Dut* dut, int reg) {
    for (int i = 0; i < 3; i++) tick(dut);    // the counters trail their events
    dut->i_l1_sys_reg = reg; dut->i_l1_sys_re = 1;
    tick(dut);
    dut->i_l1_sys_re = 0; dut->eval();
    return dut->o_l1_sys_rdata;
}

static void run(Dut* dut, const Config& cfg, int max_cycles, Trace& t) {
    g_cfg = cfg;
    g_t = nullptr;
    g_waited = 0;
    g_req_prev = false;
    dut->i_cacheable = cfg.cacheable;
    dut->i_irq = 0;
    dut->i_l1_sys_reg = 0; dut->i_l1_sys_wdata = 0;
    dut->i_l1_sys_we = 0; dut->i_l1_sys_re = 0;
    dut->i_irsp_done = 0; dut->i_rsp_fault = 0;
    dut->i_store_fault = 0; dut->i_store_fault_addr = 0;
    dut->i_rst = 1; dut->i_core_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    if (cfg.l1_on) l1_write(dut, CACHE_CTRL, 1);
    dut->i_core_rst = 0;
    g_t = &t;
    bool irq_fired = false;
    for (int c = 0; c < max_cycles; c++) {
        if (cfg.irq_cycle >= 0 && c >= cfg.irq_cycle && !irq_fired) dut->i_irq = 1;
        dut->i_store_fault      = c == cfg.store_fault_cycle;
        dut->i_store_fault_addr = cfg.store_fault_addr;
        tick(dut);
        if (dut->o_commit_we)
            t.commits.push_back({dut->o_commit_idx, dut->o_commit_data});
        if (dut->o_branch_taken)
            t.branches.push_back({dut->o_branch_pc, dut->o_branch_target});
        if (dut->o_retire_valid) {
            t.retire_ops.push_back(dut->o_retire_op_class);
            t.retire_srs.push_back(dut->o_retire_sr);
        }
        if (dut->o_predict)    t.predicted.push_back(dut->o_predict_pc);
        if (dut->o_mispredict) t.mispredicted.push_back(dut->o_branch_pc);
        if (dut->o_dc_commit) {
            t.dc_pcs.push_back(dut->o_dc_commit_pc);
            t.dc_ops.push_back(dut->o_dc_commit_op_class);
        }
        if (dut->o_fault_commit) {
            t.fault_vecs.push_back(dut->o_fault_vec);
            if (dut->o_fault_vec == VEC_EXT_IRQ) { dut->i_irq = 0; irq_fired = true; }
        }
        if (dut->o_mmu_fault_commit) {
            t.mmu_faults++;
            t.fault_vaddr = dut->o_mmu_fault_vaddr;
            t.fault_status = dut->o_mmu_fault_status;
        }
    }
}

static void check_commits(const char* tag, const std::vector<Commit>& got,
                          const std::vector<Commit>& exp) {
    char n[48];
    snprintf(n, sizeof(n), "%s_count", tag);
    check(n, (uint32_t)got.size(), (uint32_t)exp.size());
    for (size_t i = 0; i < exp.size() && i < got.size(); i++) {
        snprintf(n, sizeof(n), "%s_%zu_idx", tag, i);
        check(n, got[i].idx, exp[i].idx);
        snprintf(n, sizeof(n), "%s_%zu_data", tag, i);
        check(n, got[i].data, exp[i].data);
    }
}

// How many requests asked for a line (or a word) at this address's line.
static uint32_t reqs_of_line(const Trace& t, uint32_t a) {
    uint32_t n = 0;
    for (const Req& r : t.reqs) n += (r.addr & ~0xFu) == (a & ~0xFu);
    return n;
}
static uint32_t reqs_of_cacheable(const Trace& t, int cacheable) {
    uint32_t n = 0;
    for (const Req& r : t.reqs) n += r.cacheable == cacheable;
    return n;
}
static uint32_t count_pc(const std::vector<uint32_t>& pcs, uint32_t pc) {
    uint32_t n = 0;
    for (uint32_t p : pcs) n += p == pc;
    return n;
}
static bool committed(const Trace& t, uint32_t idx, uint32_t data) {
    for (const Commit& c : t.commits) if (c.idx == idx && c.data == data) return true;
    return false;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* dut = new Dut;

    Config uncached;
    Config cached;   cached.cacheable = true; cached.l1_on = true;
    Config l1_off;   l1_off.cacheable = true;

    const std::vector<Commit> stream_commits = {{1,0x10},{1,0x11},{2,0x55},{1,0x66}};
    g_rom.clear();
    load(RESET_PC, {
        enc_l(OP_L_LLI,  1, 0x10),   // R1 = 0x10
        enc_l(OP_L_ADDI, 1, 0x01),   // R1 = 0x11
        enc_l(OP_L_LLI,  2, 0x55),   // R2 = 0x55
        enc_r(OP_R_ADD,  1, 2, 0),   // R1 = R1 + R2 = 0x66
    });

    // ── Streaming, every fetch a word read ───────────────────────
    {
        Trace t;
        run(dut, uncached, 120, t);
        check_commits("uc_stream", t.commits, stream_commits);
        check("uc_stream_nofault", (uint32_t)t.fault_vecs.size(), 0);
        check("uc_stream_no_line_read", reqs_of_cacheable(t, 1), 0);
        check("uc_stream_first_read", t.reqs.empty() ? 0 : t.reqs[0].addr, RESET_PC);
    }

    // ── Streaming from the L1: each line read once ───────────────
    {
        Trace t;
        run(dut, cached, 120, t);
        check_commits("c_stream", t.commits, stream_commits);
        check("c_stream_nofault", (uint32_t)t.fault_vecs.size(), 0);
        check("c_stream_no_word_read", reqs_of_cacheable(t, 0), 0);
        check("c_stream_line_once", reqs_of_line(t, RESET_PC), 1);   // it was kept
        // One miss per read: a replay's lookup is not counted again.
        check("c_stream_misses", l1_read(dut, CACHE_READ_MISSES),
              (uint32_t)t.reqs.size());
        check("c_stream_hits_some", l1_read(dut, CACHE_READ_HITS) > 0, 1);
    }

    // ── L1 off on a cacheable page: line reads, nothing kept ─────
    {
        Trace t;
        run(dut, l1_off, 200, t);
        check_commits("off_stream", t.commits, stream_commits);
        check("off_stream_line_reads", reqs_of_cacheable(t, 0), 0);
        check("off_stream_reads_again", reqs_of_line(t, RESET_PC) >= 4, 1);
    }

    // ── A slot held for FIFO room ────────────────────────────────
    // The MUL holds EX, the fetch buffer fills, and a slot waits in IF2.
    {
        g_rom.clear();
        load(RESET_PC, {
            enc_l(OP_L_LLI, 1, 6),
            enc_l(OP_L_LLI, 2, 7),
            enc_r(OP_R_MUL, 1, 2, 0) | (3 << 12),    // R1=lo=42, R3=hi=0
            enc_l(OP_L_LLI, 4, 0x44),
            enc_l(OP_L_LLI, 5, 0x55),
            enc_l(OP_L_LLI, 6, 0x66),
            enc_l(OP_L_LLI, 7, 0x77),
            enc_l(OP_L_LLI, 8, 0x88),
        });
        Trace t;
        run(dut, cached, 160, t);
        check_commits("held", t.commits, {{1,6},{2,7},{1,42},{3,0},{4,0x44},{5,0x55},
                                          {6,0x66},{7,0x77},{8,0x88}});
    }

    // ── Branch redirect: wrong-path instructions never commit ────
    {
        g_rom.clear();
        uint32_t b = RESET_PC;
        load(b, {
            enc_l(OP_L_LLI, 1, 0x10),               // +0x00  R1 = 0x10
            enc_b_to(b + 0x04, b + 0x20, COND_AL),  // +0x04  BR -> +0x20
            enc_l(OP_L_LLI, 1, 0xBAD),              // +0x08  wrong path
            enc_l(OP_L_LLI, 1, 0xBAD),              // +0x0C  wrong path
        });
        load(b + 0x20, {enc_l(OP_L_LLI, 2, 0x22)}); // +0x20  target: R2 = 0x22

        // The word read of +0x08 is in flight when the branch resolves. Its
        // word must not reach the slot at the target.
        Config slow = uncached; slow.latency = 16;
        Trace t;
        run(dut, slow, 200, t);
        check_commits("uc_branch", t.commits, {{1,0x10},{2,0x22}});
        check("uc_branch_nofault", (uint32_t)t.fault_vecs.size(), 0);
        check("uc_branch_wrong_path_read",
              t.reqs.size() > 2 && t.reqs[2].addr == b + 0x08, 1);
        check("uc_branch_pc",     t.branches.empty() ? 0 : t.branches[0].pc, b + 0x04);
        check("uc_branch_target", t.branches.empty() ? 0 : t.branches[0].target, b + 0x20);
        check("uc_branch_retire_op", t.retire_ops.empty() ? 0xFF : t.retire_ops[0], OPC_ALU);
        check("uc_branch_retire_sr_s",
              t.retire_srs.empty() ? 0 : t.retire_srs[0] >> 31, 1);
    }
    {
        g_rom.clear();
        uint32_t b = RESET_PC;
        load(b, {
            enc_l(OP_L_LLI, 1, 0x10),               // +0x00  R1 = 0x10
            enc_l(OP_L_LLI, 2, 0x11),               // +0x04
            enc_l(OP_L_LLI, 3, 0x12),               // +0x08
            enc_b_to(b + 0x0C, b + 0x20, COND_AL),  // +0x0C  BR -> +0x20
            enc_l(OP_L_LLI, 1, 0xBAD),              // +0x10  wrong path, the next line
            enc_l(OP_L_LLI, 4, 0x44),               // +0x14  reached from +0x24
        });
        load(b + 0x20, {
            enc_l(OP_L_LLI, 2, 0x22),               // +0x20  target
            enc_b_to(b + 0x24, b + 0x14, COND_AL),  // +0x24  BR -> +0x14
        });

        // The next line's read is in flight when the branch resolves. The
        // read runs to its done and the L1 keeps the line, so the branch
        // back into it reads nothing more.
        Config slow = cached; slow.latency = 16;
        Trace t;
        run(dut, slow, 300, t);
        check_commits("c_branch", t.commits,
                      {{1,0x10},{2,0x11},{3,0x12},{2,0x22},{4,0x44}});
        check("c_branch_line_kept", reqs_of_line(t, b + 0x10), 1);
        bool two = t.branches.size() >= 2;
        check("c_branch_pc0",     two ? t.branches[0].pc : 0,     b + 0x0C);
        check("c_branch_target0", two ? t.branches[0].target : 0, b + 0x20);
        check("c_branch_pc1",     two ? t.branches[1].pc : 0,     b + 0x24);
        check("c_branch_target1", two ? t.branches[1].target : 0, b + 0x14);
    }

    // ── Interrupt entry via the vector fetch ─────────────────────
    {
        g_rom.clear();
        uint32_t base = RESET_PC;
        load(base, {
            enc_r(OP_R_EI,  0, 0, 0),    // enable interrupts (one-instruction shadow)
            enc_l(OP_L_LLI, 1, 0x01),    // shadowed instruction (runs masked)
            enc_l(OP_L_LLI, 1, 0x02),
            enc_l(OP_L_LLI, 1, 0x03),
            enc_l(OP_L_LLI, 1, 0x04),
        });
        load(base + 0x100, {
            enc_l(OP_L_LLI, 5, 0xABC),   // handler marker
        });
        g_rom[VEC_EXT_IRQ << 2] = base + 0x100;   // vector_table[VEC_EXT_IRQ] = handler

        Config irq = cached; irq.irq_cycle = 0;
        Trace t;
        run(dut, irq, 200, t);

        bool saw_irq_fault = false;
        for (uint32_t v : t.fault_vecs) if (v == VEC_EXT_IRQ) saw_irq_fault = true;
        check("irq_fault", saw_irq_fault ? 1 : 0, 1);
        check("irq_ei_dc_pc", t.dc_pcs.empty() ? 0 : t.dc_pcs[0], base);
        check("irq_ei_dc_op", t.dc_ops.empty() ? 0xFF : t.dc_ops[0], OPC_EI);
        check("irq_handler_ran", committed(t, 5, 0xABC), 1);

        bool vec_word_read = false;
        for (const Req& r : t.reqs)
            if (r.addr == (VEC_EXT_IRQ << 2)) vec_word_read = r.cacheable == 0;
        check("irq_vector_word_read", vec_word_read, 1);
    }

    // ── A fault with a read in flight: the vector read waits ─────
    {
        g_rom.clear();
        uint32_t base = RESET_PC;
        load(base, {enc_r(OP_R_RESERVED, 1, 2, 0)});   // reserved opcode -> VEC_ILLEGAL
        load(base + 0x100, {enc_l(OP_L_LLI, 5, 0xABC)});
        g_rom[VEC_ILLEGAL << 2] = base + 0x100;

        Config slow = uncached; slow.latency = 16;
        Trace t;
        run(dut, slow, 200, t);
        check("wait_fault", t.fault_vecs.empty() ? 0xFF : t.fault_vecs[0], VEC_ILLEGAL);
        check("wait_killed_read", t.reqs.size() > 1 ? t.reqs[1].addr : 0, base + 4);
        check("wait_vector_read", t.reqs.size() > 2 ? t.reqs[2].addr : 0,
              VEC_ILLEGAL << 2);
        check("wait_handler_ran", committed(t, 5, 0xABC), 1);
    }

    // ── A fetch bus fault: a word read and a line read ───────────
    for (int pass = 0; pass < 2; pass++) {
        const char* tag = pass ? "bf_line" : "bf_word";
        char n[48];
        g_rom.clear();
        uint32_t base = RESET_PC, target = 0xFFFE0040;
        load(base, {
            enc_l(OP_L_LLI, 1, 0x11),
            enc_b_to(base + 4, target, COND_AL),
        });
        load(base + 0x100, {enc_l(OP_L_LLI, 5, 0xABC)});
        g_rom[VEC_BUS_FAULT << 2] = base + 0x100;

        Trace t;
        run(dut, pass ? cached : uncached, 200, t);
        snprintf(n, sizeof(n), "%s_vec", tag);
        check(n, t.fault_vecs.empty() ? 0xFF : t.fault_vecs[0], VEC_BUS_FAULT);
        snprintf(n, sizeof(n), "%s_one_fault", tag);
        check(n, t.mmu_faults, 1);
        snprintf(n, sizeof(n), "%s_vaddr", tag);
        check(n, t.fault_vaddr, target);
        snprintf(n, sizeof(n), "%s_status", tag);
        check(n, t.fault_status, FAULT_BUS | FSTAT_X);
        snprintf(n, sizeof(n), "%s_handler_ran", tag);
        check(n, committed(t, 5, 0xABC), 1);
    }

    // ── A deferred bus fault: taken at the next boundary, unmasked ──
    {
        g_rom.clear();
        uint32_t base = RESET_PC;
        const uint32_t n_insns = 40;
        std::vector<uint32_t> prog;
        for (uint32_t k = 1; k <= n_insns; k++) prog.push_back(enc_l(OP_L_LLI, 1, k));
        load(base, prog);
        load(base + 0x100, {
            enc_r(OP_R_RDSPR, 6, 0, 0) | (SPR_EPC << 12),   // R6 = EPC
            enc_l(OP_L_LLI, 5, 0xABC),
        });
        g_rom[VEC_BUS_FAULT << 2] = base + 0x100;

        Config df = cached;                // SR.I stays clear from reset
        df.store_fault_cycle = 40;
        df.store_fault_addr  = 0x00ABC124;
        Trace t;
        run(dut, df, 300, t);
        check("df_vec", t.fault_vecs.empty() ? 0xFF : t.fault_vecs[0], VEC_BUS_FAULT);
        check("df_one_entry", t.fault_vecs.size(), 1);
        check("df_one_latch", t.mmu_faults, 1);
        check("df_faddr",     t.fault_vaddr, 0x00ABC124);
        check("df_fstat",     t.fault_status, FAULT_DEFERRED_BUS | FSTAT_W);
        check("df_handler_ran", committed(t, 5, 0xABC), 1);

        // In order: the R1 writes that committed are the program's first ones,
        // and EPC is the one after them.
        uint32_t ran = 0, epc = 0;
        bool saw_epc = false;
        for (const Commit& c : t.commits) {
            if (c.idx == 1 && !saw_epc) ran++;
            if (c.idx == 6) { epc = c.data; saw_epc = true; }
        }
        check("df_mid_stream", ran > 0 && ran < n_insns, 1);
        check("df_epc", epc, base + 4 * ran);
    }

    // ── A counted loop: the branch back predicted once trained ───
    // SUBI sets the flags BNE reads. The direction table starts strongly
    // not taken, so the BNE mispredicts twice while its counter climbs, is
    // predicted taken from then on, and mispredicts once more at the exit.
    // In the three-instruction body the BNE is fetched again as its previous
    // pass trains its entry, so its read meets that write. With the L1 off
    // nothing hits, so nothing is predicted and EX redirects behind every
    // taken BNE.
    for (int len = 3; len <= 4; len++) {
        const uint32_t N = 8;
        char tag[32];
        g_rom.clear();
        uint32_t b = RESET_PC;
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI,  2, N),                  // R2 = N
            enc_l(OP_L_LLI,  3, 0),                  // R3 = 0
            enc_l(OP_L_ADDI, 3, 1),                  // loop: R3 += 1
        };
        if (len == 4) prog.push_back(enc_l(OP_L_LLI, 5, 0x55));
        prog.push_back(enc_l(OP_L_SUBI, 2, 1));      // R2 -= 1, flags
        const uint32_t bne = b + 4 * prog.size();
        prog.push_back(enc_b_to(bne, b + 0x08, COND_NE));
        prog.push_back(enc_l(OP_L_LLI, 4, 0x44));    // after the loop
        load(b, prog);

        std::vector<Commit> exp = {{2, N}, {3, 0}};
        for (uint32_t i = 1; i <= N; i++) {
            exp.push_back({3, i});
            if (len == 4) exp.push_back({5, 0x55});
            exp.push_back({2, N - i});
        }
        exp.push_back({4, 0x44});

        Trace t;
        run(dut, cached, 400, t);
        snprintf(tag, sizeof(tag), "c_loop%d", len);
        check_commits(tag, t.commits, exp);
        snprintf(tag, sizeof(tag), "c_loop%d_mispredicts", len);
        check(tag, count_pc(t.mispredicted, bne), 3);
        snprintf(tag, sizeof(tag), "c_loop%d_predicted", len);
        check(tag, count_pc(t.predicted, bne), N - 2);

        Trace u;
        run(dut, uncached, 1200, u);
        snprintf(tag, sizeof(tag), "uc_loop%d", len);
        check_commits(tag, u.commits, exp);
        snprintf(tag, sizeof(tag), "uc_loop%d_none_predicted", len);
        check(tag, (uint32_t)u.predicted.size(), 0);
        snprintf(tag, sizeof(tag), "uc_loop%d_mispredicts", len);
        check(tag, count_pc(u.mispredicted, bne), N - 1);
    }

    // ── Calls and returns: predicted from the word and the stack ──
    // BL redirects from its own word and pushes its next PC; the JMP R13 in
    // the function returns to the top of the stack. Neither is ever
    // mispredicted, and each return goes back behind the call it ends.
    {
        const uint32_t N = 4;
        g_rom.clear();
        uint32_t b = RESET_PC;
        load(b, {
            enc_l(OP_L_LLI,  2, N),                  // +0x00  R2 = N
            enc_b_to(b + 0x04, b + 0x40, COND_BL),   // +0x04  loop: BL func
            enc_l(OP_L_SUBI, 2, 1),                  // +0x08  R2 -= 1, flags
            enc_b_to(b + 0x0C, b + 0x04, COND_NE),   // +0x0C  BNE loop
            enc_l(OP_L_LLI,  4, 0x44),               // +0x10  after the loop
        });
        load(b + 0x40, {
            enc_l(OP_L_LLI, 3, 0x33),                // +0x40  func
            enc_l(OP_L_JMP, REG_LR, 0),              // +0x44  RET
        });
        const uint32_t bl = b + 0x04, ret = b + 0x44;
        std::vector<Commit> exp = {{2, N}};
        for (uint32_t i = 1; i <= N; i++) {
            exp.push_back({REG_LR, b + 0x08});
            exp.push_back({3, 0x33});
            exp.push_back({2, N - i});
        }
        exp.push_back({4, 0x44});

        Trace t;
        run(dut, cached, 600, t);
        check_commits("call", t.commits, exp);
        check("call_bl_predicted",     count_pc(t.predicted, bl), N);
        check("call_bl_mispredicts",   count_pc(t.mispredicted, bl), 0);
        check("call_ret_predicted",    count_pc(t.predicted, ret), N);
        check("call_ret_mispredicts",  count_pc(t.mispredicted, ret), 0);
    }

    // ── A predicted transfer waiting for FIFO room ───────────────
    // A MUL holds EX and the fetch buffer fills behind it. Across the
    // padding, the loop's BNE, trained by its third pass, waits in IF2 for
    // room while predicted taken. IF1 steers by the prediction only as the
    // BNE leaves, so the waiting BNE is never discarded.
    for (int pad = 0; pad <= 4; pad++) {
        const uint32_t N = 6;
        char tag[32];
        snprintf(tag, sizeof(tag), "held_bne_%d", pad);
        g_rom.clear();
        uint32_t b = RESET_PC;
        std::vector<uint32_t> prog = {
            enc_l(OP_L_LLI, 2, N),                   // R2 = N
            enc_l(OP_L_LLI, 7, 5),                   // R7 = 5
            enc_l(OP_L_LLI, 6, 3),                   // loop: R6 = 3
            enc_r(OP_R_MUL, 6, 7, 0) | (9 << 12),    // R6 = lo = 15, R9 = hi = 0
        };
        for (int k = 0; k < pad; k++) prog.push_back(enc_l(OP_L_LLI, 5, 0x55));
        prog.push_back(enc_l(OP_L_SUBI, 2, 1));      // R2 -= 1, flags
        uint32_t bne = b + 4 * prog.size();
        prog.push_back(enc_b_to(bne, b + 0x08, COND_NE));
        prog.push_back(enc_l(OP_L_LLI, 4, 0x44));
        load(b, prog);

        std::vector<Commit> exp = {{2, N}, {7, 5}};
        for (uint32_t i = 1; i <= N; i++) {
            exp.push_back({6, 3});
            exp.push_back({6, 15});
            exp.push_back({9, 0});
            for (int k = 0; k < pad; k++) exp.push_back({5, 0x55});
            exp.push_back({2, N - i});
        }
        exp.push_back({4, 0x44});

        Trace t;
        run(dut, cached, 1200, t);
        check_commits(tag, t.commits, exp);
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

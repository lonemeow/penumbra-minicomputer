// Verilator testbench for penumbra3_core.
//
// Exercises the whole core integration -- IF1/IF2 + elastic fetch buffer +
// spine + vecfetch + irq -- driven through the real instruction-fetch port. A
// sync-read instruction ROM models the I-side front port: the address IF1 drives
// is latched at the posedge under the o_fetch_en clock-enable and the word is
// presented the next cycle, paired with the IF1/IF2 register (the BRAM timing
// the front end is built around). The D-side verdict inputs are tied benign --
// the programs are ALU + branch only, so no load/store consults them.
//
// Coverage:
//   - streaming + in-order commit: an ALU chain fetched through the front end
//     commits the expected values (front-end <-> spine integration)
//   - branch redirect: a taken branch steers the fetch stream past the
//     wrong-path instructions, which must never commit (the redirect arm of the
//     front-end composition)
//   - interrupt entry: an external IRQ injects at the EX frontier, the vector
//     fetch reads the handler address, and the handler runs (irq + vecfetch +
//     the vector-fetch arm of the front-end composition)
//
// ERET and WRSYS-resync redirects share the same composition arm as the branch
// and vector-fetch cases and are exercised at the machine + program-suite level
// (the spine unit test already covers their commit-side behavior).

#include <cstdio>
#include <cstdint>
#include <vector>
#include <map>
#include "Vpenumbra3_core.h"
#include "verilated.h"

// ── ISA constants (penumbra_pkg / penumbra3_pkg) ─────────────────
enum { OP_R_ADD = 0, OP_R_EI = 28 };
enum { OP_L_LLI = 0, OP_L_ADDI = 3 };
enum { COND_AL = 0 };
enum { VEC_EXT_IRQ = 9 };

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

// ── Sync-read instruction ROM ────────────────────────────────────
// Unfilled addresses read a branch-to-self (target = PC + 0): the PC parks
// there committing nothing, so a program halts simply by running off its end.
static const uint32_t ROM_FILLER = (3u << 30);   // enc_b(COND_AL, 0)
static std::map<uint32_t, uint32_t> g_rom;
static uint32_t g_addr_q;

static uint32_t rom_at(uint32_t a) {
    auto it = g_rom.find(a);
    return it != g_rom.end() ? it->second : ROM_FILLER;
}
static void load(uint32_t base, const std::vector<uint32_t>& prog) {
    for (size_t i = 0; i < prog.size(); i++) g_rom[base + i * 4] = prog[i];
}

// Present the benign D-side verdicts (no load/store in these programs, so they
// are never actually consumed -- just held off the fault path).
static void dside_benign(Vpenumbra3_core* dut) {
    dut->i_fetch_busy = 0; dut->i_fetch_fault = 0;
    dut->i_fetch_mmu_fault = 0; dut->i_fetch_mmu_fault_status = 0;
    dut->i_translate_hit = 1; dut->i_translate_paddr = 0;
    dut->i_translate_cacheable = 1;
    dut->i_translate_miss_fault = 0; dut->i_translate_prot_fault = 0;
    dut->i_dcache_hit = 1; dut->i_dcache_rdata = 0;
    dut->i_fill_done = 0; dut->i_fill_data = 0; dut->i_fill_fault = 0;
    dut->i_sys_rdata = 0;
}

// One clock with the sync-read instruction ROM folded in. A sync-read BRAM
// holds its data output stable across the posedge (= rom of the address latched
// at the *previous* edge) and latches the new address at this edge. So present
// this cycle's word, evaluate through the edge with it held, and only then
// update the address register from the pre-edge o_fetch_addr (gated by the
// o_fetch_en clock-enable) -- otherwise the FIFO would capture a word paired
// with the wrong PC.
static void tick(Vpenumbra3_core* dut) {
    dut->i_fetch_rdata = rom_at(g_addr_q);
    dside_benign(dut);
    dut->i_clk = 0; dut->eval();
    uint32_t latched = dut->o_fetch_en ? dut->o_fetch_addr : g_addr_q;
    dut->i_clk = 1; dut->eval();
    g_addr_q = latched;
}

struct Commit { uint32_t idx, data; };

static void run(Vpenumbra3_core* dut, int max_cycles,
                std::vector<Commit>& commits, std::vector<uint32_t>& fault_vecs,
                int irq_cycle) {
    g_addr_q = RESET_PC;
    dut->i_irq = 0; dut->i_timer_irq = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    bool irq_fired = false;
    for (int c = 0; c < max_cycles; c++) {
        if (irq_cycle >= 0 && c >= irq_cycle && !irq_fired) dut->i_irq = 1;
        tick(dut);
        if (dut->o_commit_we) commits.push_back({dut->o_commit_idx, dut->o_commit_data});
        if (dut->o_fault_commit) {
            fault_vecs.push_back(dut->o_fault_vec);
            if (dut->o_fault_vec == VEC_EXT_IRQ) { dut->i_irq = 0; irq_fired = true; }
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

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_core* dut = new Vpenumbra3_core;

    // ── Streaming + in-order commit ──────────────────────────────
    {
        g_rom.clear();
        load(RESET_PC, {
            enc_l(OP_L_LLI,  1, 0x10),   // R1 = 0x10
            enc_l(OP_L_ADDI, 1, 0x01),   // R1 = 0x11
            enc_l(OP_L_LLI,  2, 0x55),   // R2 = 0x55
            enc_r(OP_R_ADD,  1, 2, 0),   // R1 = R1 + R2 = 0x66
        });
        std::vector<Commit> got; std::vector<uint32_t> flt;
        run(dut, 80, got, flt, -1);
        check_commits("stream", got, {{1,0x10},{1,0x11},{2,0x55},{1,0x66}});
        check("stream_nofault", (uint32_t)flt.size(), 0);
    }

    // ── Branch redirect: wrong-path instructions never commit ────
    {
        g_rom.clear();
        uint32_t b = RESET_PC;
        load(b, {
            enc_l(OP_L_LLI, 1, 0x10),               // +0x00  R1 = 0x10
            enc_b_to(b + 0x04, b + 0x10, COND_AL),  // +0x04  BR -> +0x10
            enc_l(OP_L_LLI, 1, 0xBAD),              // +0x08  wrong path
            enc_l(OP_L_LLI, 1, 0xBAD),              // +0x0C  wrong path
            enc_l(OP_L_LLI, 2, 0x22),               // +0x10  target: R2 = 0x22
        });
        std::vector<Commit> got; std::vector<uint32_t> flt;
        run(dut, 80, got, flt, -1);
        check_commits("branch", got, {{1,0x10},{2,0x22}});
        check("branch_nofault", (uint32_t)flt.size(), 0);
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

        std::vector<Commit> got; std::vector<uint32_t> flt;
        run(dut, 120, got, flt, /*irq_cycle=*/0);

        bool saw_irq_fault = false;
        for (uint32_t v : flt) if (v == VEC_EXT_IRQ) saw_irq_fault = true;
        check("irq_fault", saw_irq_fault ? 1 : 0, 1);

        bool saw_handler = false;
        for (const Commit& c : got) if (c.idx == 5 && c.data == 0xABC) saw_handler = true;
        check("irq_handler_ran", saw_handler ? 1 : 0, 1);
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

// Verilator testbench for penumbra3_if2_stage (via penumbra3_if2_stage_test).
//
// Drives the IF1/IF2 register, the L1 and MMU verdicts, the fetch replay unit's
// delivery and the fetch-FIFO enqueue handshake (i_enq_ready), checking the
// enqueue outputs, the miss report and the hold against the IF2 contract. gen3
// has no IF2/ID output register -- the enqueue is combinational and the FIFO
// flops it -- so the outputs are read in the *same* cycle, and "delivery" is
// the enqueue handshake (o_enq_valid & i_enq_ready).
//
//   - a hit enqueues the same cycle, the hit way's word selected
//   - a bubble reports nothing
//   - a miss is reported and enqueues nothing, and does not hold IF1
//   - a slot with no FIFO room holds IF1, whatever its verdict
//   - a delivered word enqueues with no hit and in place of a hit, and a
//     delivered fault tags the slot
//   - a flush discards the slot: no enqueue and no miss
//   - a misaligned PC or a TLB fault tags the slot with no miss
//   - each cacheable fetch is counted once: a replay or a held slot adds nothing

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_if2_stage_test.h"
#include "verilated.h"

typedef Vpenumbra3_if2_stage_test Dut;

// op_class (penumbra3_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2 };
enum { VEC_BUS_FAULT = 0, VEC_ALIGN = 8 };
enum { FAULT_BUS = 4, FSTAT_X = 1 << 10 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Dut* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// ── Instruction word encoders (distinct op_class per format) ─────
static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
}
static uint32_t enc_m(int L, int sz, int se, int rd, int rb, uint16_t off) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22) | ((rb & 0xF) << 18) | ((uint32_t)off << 2);
}
static const uint32_t WORD_ALU   = enc_r(0, 1, 2, 0);            // ADD  -> OPC_ALU
static const uint32_t WORD_LOAD  = enc_m(1, 2, 0, 5, 6, 0);      // LDW  -> OPC_LOAD
static const uint32_t WORD_STORE = enc_m(0, 2, 0, 5, 6, 0);      // STW  -> OPC_STORE

static void clear(Dut* dut) {
    dut->i_pc = 0; dut->i_next_pc = 4; dut->i_valid = 0;
    dut->i_way_hit = 0; dut->i_way_word = 0; dut->i_hit = 0;
    dut->i_cacheable = 1; dut->i_user_mode = 0;
    dut->i_mmu_fault = 0; dut->i_mmu_fault_status = 0;
    dut->i_replayed = 0; dut->i_deliver = 0; dut->i_deliver_word = 0;
    dut->i_deliver_fault = 0;
    dut->i_flush = 0; dut->i_enq_ready = 1;
}

// A slot whose lookup missed, with both ways holding words of another line.
static void slot(Dut* dut, uint32_t pc) {
    clear(dut);
    dut->i_pc = pc; dut->i_next_pc = pc + 4; dut->i_valid = 1;
    dut->i_way_word = ((uint64_t)WORD_LOAD << 32) | WORD_ALU;    // way 1, way 0
}
static void hit_way(Dut* dut, int way) {
    dut->i_way_hit = 1 << way; dut->i_hit = 1;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* dut = new Dut;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_enq", dut->o_enq_valid, 0);

    // ── A hit enqueues the same cycle, from the hit way ──────────
    slot(dut, 0x1000); hit_way(dut, 0); dut->eval();
    check("hit0_enq",     dut->o_enq_valid, 1);
    check("hit0_fire",    dut->o_enq_fire, 1);
    check("hit0_no_miss", dut->o_miss, 0);
    check("hit0_no_hold", dut->o_hold, 0);
    check("hit0_pc",      dut->o_enq_pc, 0x1000);
    check("hit0_next_pc", dut->o_enq_next_pc, 0x1004);
    check("hit0_class",   dut->o_enq_op_class, OPC_ALU);
    check("hit0_nofault", dut->o_enq_fault_pending, 0);

    slot(dut, 0x1004); hit_way(dut, 1); dut->eval();
    check("hit1_enq",     dut->o_enq_valid, 1);
    check("hit1_class",   dut->o_enq_op_class, OPC_LOAD);

    // ── Bubble: nothing reported ─────────────────────────────────
    clear(dut); dut->eval();
    check("bub_enq",     dut->o_enq_valid, 0);
    check("bub_no_miss", dut->o_miss, 0);
    check("bub_no_hold", dut->o_hold, 0);

    // ── A miss is reported, and IF1 is not held on it ────────────
    slot(dut, 0x2000); dut->eval();
    check("miss_reported", dut->o_miss, 1);
    check("miss_no_enq",   dut->o_enq_valid, 0);
    check("miss_no_hold",  dut->o_hold, 0);

    // ── No FIFO room: the slot holds IF1 and stays ───────────────
    slot(dut, 0x3000); hit_way(dut, 0); dut->i_enq_ready = 0; dut->eval();
    check("full_valid",   dut->o_enq_valid, 1);
    check("full_no_fire", dut->o_enq_fire, 0);
    check("full_hold",    dut->o_hold, 1);
    check("full_no_miss", dut->o_miss, 0);

    // A miss with no FIFO room needs no room to report.
    slot(dut, 0x3004); dut->i_enq_ready = 0; dut->eval();
    check("full_miss",      dut->o_miss, 1);
    check("full_miss_hold", dut->o_hold, 1);

    // ── A delivered word enqueues with no hit ────────────────────
    slot(dut, 0x4000);
    dut->i_deliver = 1; dut->i_deliver_word = WORD_STORE; dut->eval();
    check("deliver_enq",     dut->o_enq_valid, 1);
    check("deliver_no_miss", dut->o_miss, 0);
    check("deliver_class",   dut->o_enq_op_class, OPC_STORE);   // not way 0's word
    check("deliver_nofault", dut->o_enq_fault_pending, 0);

    // ── A delivered word wins over a hit its lookup also finds ───
    // The access's line may have gone into the L1, so the replayed lookup
    // can hit beside the delivery; the bundle is the delivered word's alone.
    slot(dut, 0x4010); hit_way(dut, 1);
    dut->i_deliver = 1; dut->i_deliver_word = WORD_STORE; dut->eval();
    check("deliver_hit1_enq",   dut->o_enq_valid, 1);
    check("deliver_hit1_class", dut->o_enq_op_class, OPC_STORE);    // not way 1's or both
    slot(dut, 0x4014); hit_way(dut, 0);
    dut->i_deliver = 1; dut->i_deliver_word = WORD_STORE; dut->eval();
    check("deliver_hit0_class", dut->o_enq_op_class, OPC_STORE);    // not way 0's

    // ── A delivered fault tags the slot ──────────────────────────
    slot(dut, 0x4100);
    dut->i_deliver = 1; dut->i_deliver_fault = 1; dut->eval();
    check("bus_enq",    dut->o_enq_valid, 1);
    check("bus_fault",  dut->o_enq_fault_pending, 1);
    check("bus_vec",    dut->o_enq_fault_vec, VEC_BUS_FAULT);
    check("bus_status", dut->o_enq_fault_status, FAULT_BUS | FSTAT_X);

    // A fault needs the delivery: a stale fault flag alone tags nothing.
    slot(dut, 0x4200); hit_way(dut, 0); dut->i_deliver_fault = 1; dut->eval();
    check("stale_fault", dut->o_enq_fault_pending, 0);

    // ── A flush discards the slot ────────────────────────────────
    slot(dut, 0x5000); hit_way(dut, 0); dut->i_flush = 1; dut->eval();
    check("flush_no_enq",  dut->o_enq_valid, 0);
    check("flush_no_fire", dut->o_enq_fire, 0);

    slot(dut, 0x5004); dut->i_flush = 1; dut->eval();
    check("flush_no_miss", dut->o_miss, 0);

    // ── Misaligned PC: fault tag, no miss ────────────────────────
    slot(dut, 0x6002); dut->eval();
    check("align_no_miss", dut->o_miss, 0);
    check("align_enq",     dut->o_enq_valid, 1);
    check("align_fault",   dut->o_enq_fault_pending, 1);
    check("align_vec",     dut->o_enq_fault_vec, VEC_ALIGN);
    check("align_pc",      dut->o_enq_pc, 0x6002);

    // ── TLB fault: composed status rides through, no miss ────────
    slot(dut, 0x7000);
    dut->i_mmu_fault = 1; dut->i_mmu_fault_status = 0x00000F01; dut->eval();
    check("tlb_no_miss", dut->o_miss, 0);
    check("tlb_enq",     dut->o_enq_valid, 1);
    check("tlb_fault",   dut->o_enq_fault_pending, 1);
    check("tlb_status",  dut->o_enq_fault_status, 0x00000F01);

    // ── Each cacheable fetch is counted once ─────────────────────
    slot(dut, 0x8000); hit_way(dut, 0); dut->eval();
    check("count_hit", dut->o_count_read, 1);
    slot(dut, 0x8000); dut->eval();
    check("count_miss", dut->o_count_read, 1);
    slot(dut, 0x8000); hit_way(dut, 0); dut->i_replayed = 1; dut->eval();
    check("count_not_replay", dut->o_count_read, 0);    // its miss was counted
    slot(dut, 0x8000); hit_way(dut, 0); dut->i_enq_ready = 0; dut->eval();
    check("count_not_held", dut->o_count_read, 0);      // counted as it leaves
    slot(dut, 0x8000); hit_way(dut, 0); dut->i_cacheable = 0; dut->eval();
    check("count_not_uncached", dut->o_count_read, 0);
    slot(dut, 0x8002); dut->eval();
    check("count_not_faulted", dut->o_count_read, 0);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

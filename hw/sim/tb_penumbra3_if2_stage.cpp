// Verilator testbench for penumbra3_if2_stage (via penumbra3_if2_stage_test).
//
// Drives the IF1/IF2 register, the I-side front port (busy + word), the MMU
// port-A verdict, and the fetch-FIFO enqueue handshake (i_enq_ready), checking
// the enqueue outputs and the fetch request against the IF2 contract. gen3 has
// no IF2/ID output register -- the enqueue is combinational and the FIFO flops
// it -- so the outputs are read in the *same* cycle, and "delivery" is the
// enqueue handshake (o_enq_valid & i_enq_ready).
//
//   - reset clears the enqueue
//   - a hit-timing word (busy low) is enqueued the same cycle, request raised
//   - a bubble raises no request and no enqueue
//   - a miss holds the request level across busy, back-pressures IF1, then
//     presents the word on the busy-drop cycle (drop-equals-valid)
//   - a completion while the FIFO is full parks in the skid: the request
//     deasserts, IF1 holds, and the *parked* word (not the stale port word)
//     is enqueued when the FIFO accepts -- verified via op_class
//   - a flush discards the slot (no enqueue) and abandons a mid-wait fill
//   - a misaligned PC faults without a request; a TLB fault rides through with
//     the MMU's composed status, no request

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_if2_stage_test.h"
#include "verilated.h"

// op_class (penumbra3_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2 };
enum { VEC_ALIGN = 8 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_if2_stage_test* dut) {
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

// Present a slot in the IF1/IF2 register with an identity (no-fault) verdict.
static void slot(Vpenumbra3_if2_stage_test* dut, uint32_t pc, uint32_t ir) {
    dut->i_pc = pc; dut->i_next_pc = pc + 4; dut->i_valid = 1;
    dut->i_ir = ir;
    dut->i_mmu_fault = 0; dut->i_mmu_fault_status = 0;
}

static void clear(Vpenumbra3_if2_stage_test* dut) {
    dut->i_pc = 0; dut->i_next_pc = 4; dut->i_valid = 0; dut->i_ir = 0;
    dut->i_mem_busy = 0; dut->i_mem_fault = 0; dut->i_user_mode = 0;
    dut->i_mmu_fault = 0; dut->i_mmu_fault_status = 0;
    dut->i_flush = 0; dut->i_enq_ready = 1;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_if2_stage_test* dut = new Vpenumbra3_if2_stage_test;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_enq", dut->o_enq_valid, 0);

    // ── Hit timing: request + enqueue the same cycle ─────────────
    clear(dut);
    slot(dut, 0x1000, WORD_ALU); dut->eval();
    check("hit_re",      dut->o_fetch_re, 1);
    check("hit_enq",     dut->o_enq_valid, 1);
    check("hit_nostall", dut->o_stall, 0);
    check("hit_pc",      dut->o_enq_pc, 0x1000);
    check("hit_class",   dut->o_enq_op_class, OPC_ALU);
    check("hit_nofault", dut->o_enq_fault_pending, 0);

    // ── Bubble slot: no request, no enqueue ──────────────────────
    clear(dut); dut->i_valid = 0; dut->eval();
    check("bub_re",  dut->o_fetch_re, 0);
    check("bub_enq", dut->o_enq_valid, 0);
    check("bub_nostall", dut->o_stall, 0);

    // ── Miss: request held across busy, word at the drop ─────────
    clear(dut);
    slot(dut, 0x2000, 0); dut->i_mem_busy = 1; dut->eval();
    check("miss_re",    dut->o_fetch_re, 1);
    check("miss_stall", dut->o_stall, 1);          // hold IF1
    check("miss_noenq", dut->o_enq_valid, 0);      // no word yet
    tick(dut); dut->eval();
    check("miss_re2",   dut->o_fetch_re, 1);       // request held
    tick(dut);
    dut->i_mem_busy = 0; dut->i_ir = WORD_LOAD;    // drop-equals-valid
    dut->eval();
    check("miss_enq",   dut->o_enq_valid, 1);
    check("miss_class", dut->o_enq_op_class, OPC_LOAD);
    check("miss_pc",    dut->o_enq_pc, 0x2000);
    check("miss_nostall", dut->o_stall, 0);        // enqueues this cycle

    // ── Skid: completion while the FIFO is full parks the word ───
    clear(dut); dut->eval(); tick(dut);            // drain to a clean state
    slot(dut, 0x3000, 0);
    dut->i_mem_busy = 1; dut->i_enq_ready = 0;      // miss, FIFO full
    dut->eval();
    check("skid_re",    dut->o_fetch_re, 1);
    check("skid_stall", dut->o_stall, 1);
    tick(dut);                                       // miss waiting
    dut->i_mem_busy = 0; dut->i_ir = WORD_LOAD;      // completes while FIFO full
    dut->eval();
    check("skid_cap_enq",   dut->o_enq_valid, 1);    // word available...
    check("skid_cap_stall", dut->o_stall, 1);        // ...but cannot enqueue
    check("skid_cap_class", dut->o_enq_op_class, OPC_LOAD);
    tick(dut);                                        // capture edge
    dut->i_ir = WORD_STORE;                          // port word goes stale
    dut->eval();
    check("skid_re_off",  dut->o_fetch_re, 0);        // completion consumed, no new request
    check("skid_enq",     dut->o_enq_valid, 1);       // parked word presented
    check("skid_stall2",  dut->o_stall, 1);           // still FIFO full
    check("skid_class",   dut->o_enq_op_class, OPC_LOAD);  // the PARKED word, not WORD_STORE
    tick(dut); dut->eval();                           // still parked
    check("skid_class2",  dut->o_enq_op_class, OPC_LOAD);
    dut->i_enq_ready = 1; dut->eval();                // FIFO accepts
    check("skid_drain_enq",   dut->o_enq_valid, 1);
    check("skid_drain_class", dut->o_enq_op_class, OPC_LOAD);
    check("skid_drain_pc",    dut->o_enq_pc, 0x3000);
    check("skid_drain_nostall", dut->o_stall, 0);     // enqueues (release)
    tick(dut);                                         // edge: enqueue fires, skid releases

    // ── Flush discards the slot: no enqueue ──────────────────────
    clear(dut);
    slot(dut, 0x4000, WORD_ALU); dut->i_flush = 1; dut->eval();
    check("flush_enq", dut->o_enq_valid, 0);

    // ── Flush mid-wait abandons the completion ───────────────────
    clear(dut);
    slot(dut, 0x5000, 0); dut->i_mem_busy = 1; dut->eval();
    check("fmw_re", dut->o_fetch_re, 1);
    tick(dut);                                        // fill in flight
    dut->i_flush = 1; dut->eval();
    check("fmw_enq_off", dut->o_enq_valid, 0);        // slot discarded
    tick(dut);
    dut->i_flush = 0; dut->i_valid = 0; dut->eval();  // IF1 bubbled the slot
    tick(dut);
    dut->i_mem_busy = 0; dut->i_ir = 0xDEADBEEF;      // fill completes into the void
    dut->eval();
    check("fmw_void", dut->o_enq_valid, 0);           // nothing enqueued

    // ── Misaligned PC: fault tag, no request ─────────────────────
    clear(dut);
    slot(dut, 0x6002, 0); dut->eval();
    check("align_re",    dut->o_fetch_re, 0);
    check("align_enq",   dut->o_enq_valid, 1);
    check("align_fault", dut->o_enq_fault_pending, 1);
    check("align_vec",   dut->o_enq_fault_vec, VEC_ALIGN);
    check("align_pc",    dut->o_enq_pc, 0x6002);

    // ── TLB fault: composed status rides through, no request ─────
    clear(dut);
    slot(dut, 0x7000, 0);
    dut->i_mmu_fault = 1; dut->i_mmu_fault_status = 0x00000F01; dut->eval();
    check("tlb_re",     dut->o_fetch_re, 0);
    check("tlb_enq",    dut->o_enq_valid, 1);
    check("tlb_fault",  dut->o_enq_fault_pending, 1);
    check("tlb_status", dut->o_enq_fault_status, 0x00000F01);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

// Verilator testbench for penumbra2_if2_stage.
//
// Drives the IF1/IF2 register, the I-side front port (busy + word), the MMU
// port-A verdict, and the pipeline handshake, checking the IF2/ID register
// and the fetch request against the IF2 contract in
// doc/internals/penumbra2/pipeline-stages.md:
//   - reset clears valid
//   - a hit-timing word (busy low on the resolve cycle) delivers in 1 cycle,
//     with the request raised on that cycle
//   - a bubble slot raises no request and delivers a bubble
//   - a busy front port (miss / pass-through wait) holds the request level,
//     back-pressures IF1, bubbles into ID, then delivers the word on the
//     busy-drop cycle (drop-equals-valid)
//   - a word completing while ID back-pressures parks in the skid: the
//     request deasserts (completion consumed), the slot waits, and the
//     skid word is delivered when ID accepts
//   - a flush gates the request off (no wrong-path fill engages) and a
//     flush landing mid-wait abandons the completion entirely
//   - a misaligned PC faults without raising a request; a TLB-faulted
//     fetch advances with the MMU's composed status, no request
//
#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_if2_stage.h"
#include "verilated.h"

enum { VEC_ALIGN = 8 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_if2_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Present a slot in the IF1/IF2 register with an identity MMU verdict.
static void slot(Vpenumbra2_if2_stage* dut, uint32_t pc, uint32_t ir) {
    dut->i_pc = pc; dut->i_next_pc = pc + 4; dut->i_valid = 1;
    dut->i_ir = ir;
    dut->i_mmu_fault = 0; dut->i_mmu_fault_status = 0;
}

static void clear(Vpenumbra2_if2_stage* dut) {
    dut->i_pc = 0; dut->i_next_pc = 4; dut->i_valid = 0; dut->i_ir = 0;
    dut->i_mem_busy = 0; dut->i_user_mode = 0;
    dut->i_mmu_fault = 0; dut->i_mmu_fault_status = 0;
    dut->i_stall_in = 0; dut->i_flush = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra2_if2_stage* dut = new Vpenumbra2_if2_stage;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_valid", dut->o_valid, 0);

    // ── Hit timing: request on the resolve cycle, 1-cycle delivery ─
    slot(dut, 0x1000, 0xAABBCCDD); dut->eval();
    check("hit_re",    dut->o_fetch_re, 1);
    check("hit_nostall", dut->o_stall, 0);
    tick(dut); dut->eval();
    check("hit_valid", dut->o_valid, 1);
    check("hit_ir",    dut->o_ir, 0xAABBCCDD);
    check("hit_pc",    dut->o_pc, 0x1000);
    check("hit_nofault", dut->o_fault_pending, 0);

    // ── Bubble slot: no request, bubble out ──────────────────────
    clear(dut); dut->eval();
    check("bub_re", dut->o_fetch_re, 0);
    tick(dut); dut->eval();
    check("bub_valid", dut->o_valid, 0);

    // ── Miss: request held level across busy, word at the drop ───
    slot(dut, 0x2000, 0); dut->i_mem_busy = 1; dut->eval();
    check("miss_re",    dut->o_fetch_re, 1);
    check("miss_stall", dut->o_stall, 1);          // hold IF1
    tick(dut); dut->eval();
    check("miss_bub",   dut->o_valid, 0);          // bubble into ID
    check("miss_re2",   dut->o_fetch_re, 1);       // request held
    tick(dut); dut->eval();
    check("miss_re3",   dut->o_fetch_re, 1);
    dut->i_mem_busy = 0; dut->i_ir = 0x11223344;   // drop-equals-valid
    dut->eval();
    check("miss_drop_stall", dut->o_stall, 0);
    tick(dut); dut->eval();
    check("miss_valid", dut->o_valid, 1);
    check("miss_ir",    dut->o_ir, 0x11223344);
    check("miss_pc",    dut->o_pc, 0x2000);

    // ── Skid: completion under ID back-pressure parks the word ───
    // Drain IF2/ID to a bubble first: under i_stall_in the register holds
    // its previous content (that is what back-pressure means), so the
    // "nothing delivered yet" checks below need it empty going in.
    clear(dut); dut->eval(); tick(dut);
    slot(dut, 0x3000, 0); dut->i_mem_busy = 1; dut->i_stall_in = 1; dut->eval();
    check("skid_re",   dut->o_fetch_re, 1);
    tick(dut); dut->eval();                          // miss waiting, ID stalled
    dut->i_mem_busy = 0; dut->i_ir = 0x55667788;     // completes while stalled
    dut->eval();
    tick(dut); dut->eval();                          // capture edge
    check("skid_re_off", dut->o_fetch_re, 0);        // completion consumed
    check("skid_hold",   dut->o_valid, 0);           // nothing delivered yet
    dut->i_ir = 0xBADBADBA;                          // port word goes stale (S_IDLE)
    dut->eval(); tick(dut); dut->eval();             // still stalled: parked
    check("skid_re_off2", dut->o_fetch_re, 0);
    dut->i_stall_in = 0; dut->eval();                // ID accepts
    tick(dut); dut->eval();
    check("skid_valid", dut->o_valid, 1);
    check("skid_ir",    dut->o_ir, 0x55667788);      // the parked word, not the port
    check("skid_pc",    dut->o_pc, 0x3000);

    // ── Flush gates the request: no wrong-path fill engages ──────
    clear(dut);
    slot(dut, 0x4000, 0); dut->i_flush = 1; dut->i_mem_busy = 0; dut->eval();
    check("flush_re", dut->o_fetch_re, 0);
    tick(dut); dut->eval();
    check("flush_bub", dut->o_valid, 0);

    // ── Flush mid-wait abandons the completion ───────────────────
    clear(dut);
    slot(dut, 0x5000, 0); dut->i_mem_busy = 1; dut->eval();
    check("fmw_re", dut->o_fetch_re, 1);
    tick(dut); dut->eval();                          // fill in flight
    dut->i_flush = 1; dut->eval();
    check("fmw_re_off", dut->o_fetch_re, 0);         // request dropped with the kill
    check("fmw_stall",  dut->o_stall, 1);            // busy still holds IF1
    tick(dut);
    dut->i_flush = 0; dut->i_valid = 0; dut->eval(); // IF1 bubbled the slot
    tick(dut); dut->eval();
    dut->i_mem_busy = 0; dut->i_ir = 0xDEADBEEF;     // fill completes into the void
    dut->eval();
    tick(dut); dut->eval();
    check("fmw_void", dut->o_valid, 0);              // nothing delivered

    // ── Misaligned PC: fault tag, no request ─────────────────────
    clear(dut);
    slot(dut, 0x6002, 0); dut->eval();
    check("align_re", dut->o_fetch_re, 0);
    tick(dut); dut->eval();
    check("align_valid", dut->o_valid, 1);
    check("align_fault", dut->o_fault_pending, 1);
    check("align_vec",   dut->o_fault_vec, VEC_ALIGN);

    // ── TLB fault: composed status rides through, no request ─────
    clear(dut);
    slot(dut, 0x7000, 0);
    dut->i_mmu_fault = 1; dut->i_mmu_fault_status = 0x00000F01; dut->eval();
    check("tlb_re", dut->o_fetch_re, 0);
    tick(dut); dut->eval();
    check("tlb_valid",  dut->o_valid, 1);
    check("tlb_fault",  dut->o_fault_pending, 1);
    check("tlb_status", dut->o_fault_status, 0x00000F01);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

// Verilator testbench for penumbra3_if1_stage.
//
// Drives the handshake (back-pressure, front-port busy), the redirect/flush/
// fetch-stop actors, and checks the launch interface and the IF1/IF2 register
// against the IF1 contract:
//   - reset drives RESET_PC with the launch enabled, IF1/IF2 invalid
//   - free streaming advances PC by 4 each cycle and marks fetches valid
//   - back-pressure (i_stall_in) holds PC and the IF1/IF2 register, launch off
//   - a busy front port (i_mem_busy) holds exactly like back-pressure -- PC
//     must not advance past a fetch whose launch was suppressed
//   - a redirect steers PC at the edge and bubbles the in-flight fetch
//   - a redirect while busy steers PC but holds the target's launch until
//     the busy drop, then launches from the steered PC
//   - i_fetch_stop freezes PC at the boundary and bubbles the output

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_if1_stage.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_if1_stage* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void clear(Vpenumbra3_if1_stage* dut) {
    dut->i_stall_in = 0; dut->i_mem_busy = 0;
    dut->i_redirect = 0; dut->i_redirect_pc = 0;
    dut->i_flush = 0; dut->i_fetch_stop = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_if1_stage* dut = new Vpenumbra3_if1_stage;   // RESET_PC = 0

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_valid",   dut->o_valid, 0);
    check("rst_addr",    dut->o_fetch_addr, 0);
    check("rst_en",      dut->o_fetch_en, 1);

    // ── Free streaming: PC advances, fetches marked valid ────────
    tick(dut); dut->eval();
    check("run_pc0",     dut->o_pc, 0);
    check("run_valid0",  dut->o_valid, 1);
    check("run_next0",   dut->o_next_pc, 4);
    check("run_addr1",   dut->o_fetch_addr, 4);
    tick(dut); dut->eval();
    check("run_pc1",     dut->o_pc, 4);
    check("run_addr2",   dut->o_fetch_addr, 8);

    // ── Back-pressure holds PC, register, and the launch ─────────
    dut->i_stall_in = 1; dut->eval();
    check("stall_en",    dut->o_fetch_en, 0);
    tick(dut); dut->eval();
    check("stall_addr",  dut->o_fetch_addr, 8);   // PC held
    check("stall_pc",    dut->o_pc, 4);           // IF1/IF2 held
    check("stall_valid", dut->o_valid, 1);
    dut->i_stall_in = 0; dut->eval();
    check("unstall_en",  dut->o_fetch_en, 1);
    tick(dut); dut->eval();
    check("unstall_pc",  dut->o_pc, 8);           // resumed

    // ── Busy front port holds exactly like back-pressure ─────────
    // (PC must not advance past a fetch whose launch was suppressed.)
    dut->i_mem_busy = 1; dut->eval();
    check("busy_en",     dut->o_fetch_en, 0);
    tick(dut); dut->eval();
    check("busy_addr",   dut->o_fetch_addr, 12);  // PC held
    check("busy_pc",     dut->o_pc, 8);           // IF1/IF2 held
    tick(dut); dut->eval();
    check("busy_addr2",  dut->o_fetch_addr, 12);  // still held
    dut->i_mem_busy = 0; dut->eval();
    check("drop_en",     dut->o_fetch_en, 1);     // launch fires at the drop
    tick(dut); dut->eval();
    check("drop_pc",     dut->o_pc, 12);

    // ── Redirect with the port free: steer + bubble, launch flows ─
    dut->i_redirect = 1; dut->i_redirect_pc = 0x100; dut->eval();
    check("redir_en",    dut->o_fetch_en, 1);     // read may advance
    tick(dut);
    dut->i_redirect = 0; dut->eval();
    check("redir_addr",  dut->o_fetch_addr, 0x100); // PC steered
    check("redir_bub",   dut->o_valid, 0);          // in-flight fetch discarded
    tick(dut); dut->eval();
    check("redir_pc",    dut->o_pc, 0x100);
    check("redir_valid", dut->o_valid, 1);

    // ── Redirect while busy: steer now, launch at the busy drop ──
    // The wrong-path transaction must finish before any new lookup; the
    // target waits, parked in PC, with the launch gated.
    dut->i_mem_busy = 1; dut->eval();
    dut->i_redirect = 1; dut->i_redirect_pc = 0x200; dut->eval();
    check("rb_en",       dut->o_fetch_en, 0);     // busy outranks the redirect
    tick(dut);
    dut->i_redirect = 0; dut->eval();
    check("rb_addr",     dut->o_fetch_addr, 0x200); // PC steered anyway
    check("rb_bub",      dut->o_valid, 0);
    tick(dut); dut->eval();                          // fill still draining
    check("rb_hold_en",  dut->o_fetch_en, 0);
    check("rb_hold",     dut->o_fetch_addr, 0x200);
    check("rb_hold_bub", dut->o_valid, 0);           // nothing launched: no valid fetch
    dut->i_mem_busy = 0; dut->eval();
    check("rb_drop_en",  dut->o_fetch_en, 1);        // held launch fires
    tick(dut); dut->eval();
    check("rb_pc",       dut->o_pc, 0x200);
    check("rb_valid",    dut->o_valid, 1);

    // ── Fetch-stop freezes PC at the boundary and bubbles ────────
    dut->i_fetch_stop = 1; dut->eval();
    uint32_t boundary = dut->o_fetch_addr;
    tick(dut); dut->eval();
    check("fs_addr",     dut->o_fetch_addr, boundary);  // frozen for EPC capture
    check("fs_bub",      dut->o_valid, 0);
    dut->i_fetch_stop = 0; dut->eval();
    tick(dut); dut->eval();
    check("fs_resume",   dut->o_pc, boundary);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

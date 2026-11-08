// Verilator testbench for penumbra3_vecfetch.
//
// Walks the exception vector-fetch FSM against its contract:
//   - reset leaves it dormant (no ownership, no redirect)
//   - a fault commit starts entry: DRIVE asks for vec<<2, WAIT waits for the
//     read's done, and the captured handler word drives a registered redirect
//     one cycle after it
//   - the FSM owns the fetch side (o_active) continuously through the redirect
//     cycle
//   - a read in flight defers the request until it falls, and its done is not
//     taken for the table's

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_vecfetch.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_vecfetch* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void clear(Vpenumbra3_vecfetch* dut) {
    dut->i_fault_commit = 0; dut->i_fault_vec = 0;
    dut->i_busy = 0; dut->i_access_done = 0; dut->i_access_word = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_vecfetch* dut = new Vpenumbra3_vecfetch;

    // ── Reset: dormant ───────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_active",   dut->o_active, 0);
    check("rst_redirect", dut->o_redirect, 0);

    // ── Nothing in flight: IDLE -> DRIVE -> WAIT -> redirect ─────
    // Cycle 0 (IDLE): pulse the fault commit, vector 5.
    dut->i_fault_commit = 1; dut->i_fault_vec = 5; dut->eval();
    check("launch_active", dut->o_active, 0);   // still dormant this cycle
    tick(dut);
    dut->i_fault_commit = 0; dut->eval();

    // Cycle 1 (DRIVE): nothing in flight -> the request, address = vec<<2 = 0x14.
    check("drive_active", dut->o_active, 1);
    check("drive_req",    dut->o_req, 1);
    check("drive_addr",   dut->o_req_addr, 5 << 2);
    tick(dut);

    // Cycle 2 (WAIT): the read is the unit's; the request was one cycle.
    dut->i_busy = 1; dut->eval();
    check("wait_active", dut->o_active, 1);
    check("wait_req",    dut->o_req, 0);
    tick(dut);

    // The done, with the handler word.
    dut->i_access_done = 1; dut->i_access_word = 0xDEADBEEF; dut->eval();
    tick(dut);
    dut->i_access_done = 0; dut->i_busy = 0; dut->i_access_word = 0;

    // Registered redirect with the captured handler word; the FSM still owns
    // the fetch side for this one cycle.
    dut->eval();
    check("redir_pulse",  dut->o_redirect, 1);
    check("redir_pc",     dut->o_redirect_pc, 0xDEADBEEF);
    check("redir_active", dut->o_active, 1);
    tick(dut);

    // Fully dormant again.
    dut->eval();
    check("done_active",   dut->o_active, 0);
    check("done_redirect", dut->o_redirect, 0);

    // ── A read in flight: DRIVE defers, and its done is not the table's ─
    // Cycle 0 (IDLE): fault commit, vector 8, a killed fetch's read still up.
    dut->i_busy = 1;
    dut->i_fault_commit = 1; dut->i_fault_vec = 8; dut->eval();
    tick(dut);
    dut->i_fault_commit = 0;

    // DRIVE with the read in flight: no request, FSM holds in DRIVE.
    dut->eval();
    check("bd_req0",  dut->o_req, 0);
    check("bd_addr",  dut->o_req_addr, 8 << 2);     // address is driven regardless
    tick(dut); dut->eval();
    check("bd_req1",  dut->o_req, 0);               // still held in DRIVE

    // The killed fetch's done: no redirect comes of it.
    dut->i_access_done = 1; dut->i_access_word = 0x0BAD0BAD; dut->eval();
    check("bd_done_req", dut->o_req, 0);
    tick(dut);
    dut->i_access_done = 0; dut->i_access_word = 0; dut->i_busy = 0; dut->eval();
    check("bd_done_no_redirect", dut->o_redirect, 0);

    // Read gone: the request fires, advance to WAIT.
    check("bd_req", dut->o_req, 1);
    tick(dut);

    // WAIT: no redirect until the done.
    dut->i_busy = 1; dut->eval();
    check("bw_redirect", dut->o_redirect, 0);
    tick(dut); dut->eval();
    check("bw_hold_active", dut->o_active, 1);

    dut->i_access_done = 1; dut->i_access_word = 0x1234; dut->eval();
    tick(dut);
    dut->i_access_done = 0; dut->i_busy = 0;
    dut->eval();
    check("bw_redir_pulse", dut->o_redirect, 1);
    check("bw_redir_pc",    dut->o_redirect_pc, 0x1234);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

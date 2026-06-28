// Verilator testbench for penumbra3_vecfetch.
//
// Walks the exception vector-fetch FSM against its fetch-port contract:
//   - reset leaves it dormant (no ownership, no redirect)
//   - a fault commit launches entry: DRIVE asserts the launch + drives
//     vec<<2, WAIT holds the read request, and the captured handler word
//     drives a registered redirect one cycle after completion
//   - the FSM owns the port (o_active) continuously through the redirect cycle
//   - a busy port in DRIVE defers the launch until the port goes idle
//   - a busy port in WAIT holds the request until the completion (busy drop)

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
    dut->i_mem_rdata = 0; dut->i_mem_busy = 0;
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

    // ── Launch with the port idle: IDLE -> DRIVE -> WAIT -> redirect ─
    // Cycle 0 (IDLE): pulse the fault commit, vector 5.
    dut->i_fault_commit = 1; dut->i_fault_vec = 5; dut->eval();
    check("launch_active", dut->o_active, 0);   // still dormant this cycle
    tick(dut);
    dut->i_fault_commit = 0; dut->eval();

    // Cycle 1 (DRIVE): port idle -> launch fires, address = vec<<2 = 0x14.
    check("drive_active", dut->o_active, 1);
    check("drive_en",     dut->o_fetch_en, 1);
    check("drive_addr",   dut->o_fetch_addr, 5 << 2);
    check("drive_re",     dut->o_fetch_re, 0);
    tick(dut);

    // Cycle 2 (WAIT): request held; present the handler word at completion.
    dut->i_mem_rdata = 0xDEADBEEF; dut->eval();
    check("wait_active", dut->o_active, 1);
    check("wait_re",     dut->o_fetch_re, 1);
    check("wait_en",     dut->o_fetch_en, 0);
    tick(dut);

    // Cycle 3 (IDLE): registered redirect with the captured handler word; the
    // FSM still owns the port for this one cycle.
    dut->eval();
    check("redir_pulse",  dut->o_redirect, 1);
    check("redir_pc",     dut->o_redirect_pc, 0xDEADBEEF);
    check("redir_active", dut->o_active, 1);
    tick(dut);

    // Cycle 4: fully dormant again.
    dut->eval();
    check("done_active",   dut->o_active, 0);
    check("done_redirect", dut->o_redirect, 0);

    // ── Launch with a busy port: DRIVE defers, WAIT holds ────────
    // Cycle 0 (IDLE): launch, vector 8.
    dut->i_fault_commit = 1; dut->i_fault_vec = 8; dut->eval();
    tick(dut);
    dut->i_fault_commit = 0;

    // DRIVE with the port busy: no launch, FSM holds in DRIVE.
    dut->i_mem_busy = 1; dut->eval();
    check("bd_en0",   dut->o_fetch_en, 0);
    check("bd_addr",  dut->o_fetch_addr, 8 << 2);   // address is driven regardless
    tick(dut); dut->eval();
    check("bd_en1",   dut->o_fetch_en, 0);          // still held in DRIVE
    check("bd_re",    dut->o_fetch_re, 0);

    // Port goes idle: launch fires, advance to WAIT.
    dut->i_mem_busy = 0; dut->eval();
    check("bd_launch", dut->o_fetch_en, 1);
    tick(dut);

    // WAIT with the port busy: request held, no completion yet.
    dut->i_mem_busy = 1; dut->eval();
    check("bw_re",       dut->o_fetch_re, 1);
    check("bw_redirect", dut->o_redirect, 0);
    tick(dut); dut->eval();
    check("bw_hold_re", dut->o_fetch_re, 1);        // still waiting

    // Completion: present the handler word at the busy drop.
    dut->i_mem_busy = 0; dut->i_mem_rdata = 0x1234; dut->eval();
    tick(dut);
    dut->eval();
    check("bw_redir_pulse", dut->o_redirect, 1);
    check("bw_redir_pc",    dut->o_redirect_pc, 0x1234);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

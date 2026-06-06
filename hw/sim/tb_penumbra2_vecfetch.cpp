// Verilator testbench for penumbra2_vecfetch.
//
// Drives the launch pulse and the fetched word across clock edges, checking
// the vector-fetch FSM's sequence against exception-flow.md:
//   - dormant in IDLE; a fault commit launches entry
//   - DRIVE owns the port and drives vector_table[vec<<2] with read-enable
//   - WAIT redirects PC to the handler word on i_mem_rdata, then returns
//   - the address is vec<<2 for several vectors
//
// The FSM's --assert invariants (no nested launch mid-fetch; redirect only in
// WAIT) ride along: a failed assertion aborts the sim (exit 1).

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_vecfetch.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_vecfetch* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void clear(Vpenumbra2_vecfetch* dut) {
    dut->i_fault_commit = 0; dut->i_fault_vec = 0; dut->i_mem_rdata = 0;
}

// Run one full entry for vector `vec` returning handler word `handler`; check
// the per-state outputs along the way.
static void run_entry(Vpenumbra2_vecfetch* dut, int vec, uint32_t handler,
                      const char* tag) {
    char nm[64];
    // IDLE: launch
    clear(dut);
    dut->i_fault_commit = 1; dut->i_fault_vec = vec;
    dut->eval();
    snprintf(nm, sizeof nm, "%s_idle_inactive", tag); check(nm, dut->o_active, 0);
    tick(dut);                                   // → DRIVE

    // DRIVE: owns the port, drives vec<<2 + read-enable
    clear(dut);
    dut->eval();
    snprintf(nm, sizeof nm, "%s_drive_active",   tag); check(nm, dut->o_active, 1);
    snprintf(nm, sizeof nm, "%s_drive_fetch_en", tag); check(nm, dut->o_fetch_en, 1);
    snprintf(nm, sizeof nm, "%s_drive_addr",     tag); check(nm, dut->o_fetch_addr, (uint32_t)(vec << 2));
    snprintf(nm, sizeof nm, "%s_drive_no_redir", tag); check(nm, dut->o_redirect, 0);
    tick(dut);                                   // → WAIT

    // WAIT: handler word arrives, redirect fires
    clear(dut);
    dut->i_mem_rdata = handler;
    dut->eval();
    snprintf(nm, sizeof nm, "%s_wait_active",   tag); check(nm, dut->o_active, 1);
    snprintf(nm, sizeof nm, "%s_wait_redirect", tag); check(nm, dut->o_redirect, 1);
    snprintf(nm, sizeof nm, "%s_wait_pc",       tag); check(nm, dut->o_redirect_pc, handler);
    snprintf(nm, sizeof nm, "%s_wait_no_fetch", tag); check(nm, dut->o_fetch_en, 0);
    tick(dut);                                   // → IDLE

    // IDLE again
    clear(dut);
    dut->eval();
    snprintf(nm, sizeof nm, "%s_back_idle", tag); check(nm, dut->o_active, 0);
    snprintf(nm, sizeof nm, "%s_idle_no_redir", tag); check(nm, dut->o_redirect, 0);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra2_vecfetch* dut = new Vpenumbra2_vecfetch;

    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_inactive", dut->o_active, 0);

    // VEC_ALIGN (8) → table slot 0x20, handler in ROM
    run_entry(dut, 8, 0xFFFF0024, "align");
    // A couple more vectors to cover the vec<<2 address
    run_entry(dut, 0, 0xFFFF0100, "vec0");
    run_entry(dut, 10, 0xFFFF0200, "vec10");

    // Two entries back-to-back: the FSM must be ready again immediately.
    run_entry(dut, 6, 0xCAFE0000, "again");

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

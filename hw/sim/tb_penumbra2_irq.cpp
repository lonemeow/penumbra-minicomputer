// Verilator testbench for penumbra2_irq.
//
// Drives the IRQ lines, the enable/shadow inputs, and the drain state across
// clock edges, checking the recognition + drain-and-take contract in
// doc/internals/penumbra2/exception-flow.md:
//   - eligible (line & SR.I & ~ei_shadow) stops fetch, drains, then takes
//   - ei_shadow (set by EI, cleared by the next completion) masks recognition
//   - timer outranks the external line
//   - a fault during the drain preempts the entry
//   - EPC is the boundary PC latched at recognition
//
// The unit's --assert invariants (entry only from a drained DRAIN; entry never
// coincides with a fault) ride along.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_irq.h"
#include "verilated.h"

enum { VEC_TIMER = 1, VEC_EXT_IRQ = 9 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_irq* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void clear(Vpenumbra2_irq* dut) {
    dut->i_irq = 0; dut->i_timer_irq = 0;
    dut->i_sr_i = 0; dut->i_ei_commit = 0;
    dut->i_retire_valid = 0; dut->i_dc_commit = 0;
    dut->i_pipe_busy = 0; dut->i_boundary_pc = 0;
    dut->i_fault_commit = 0; dut->i_vecf_active = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra2_irq* dut = new Vpenumbra2_irq;

    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_no_stop",  dut->o_fetch_stop, 0);
    check("reset_no_entry", dut->o_irq_entry, 0);

    // ── Basic entry: eligible → stop → drain → take ──────────────
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_boundary_pc = 0x1234; dut->i_pipe_busy = 1;
    dut->eval();
    check("elig_fetch_stop", dut->o_fetch_stop, 1);   // recognition stops fetch
    check("elig_no_entry",   dut->o_irq_entry, 0);     // pipe still busy
    tick(dut);                                          // → DRAIN (latch epc/vec)
    dut->i_pipe_busy = 1; dut->eval();
    check("drain_stop",   dut->o_fetch_stop, 1);
    check("drain_noentry", dut->o_irq_entry, 0);
    dut->i_pipe_busy = 0; dut->eval();                 // pipe empties
    check("drain_entry", dut->o_irq_entry, 1);
    check("drain_vec",   dut->o_irq_vec, VEC_EXT_IRQ);
    check("drain_epc",   dut->o_irq_epc, 0x1234);
    tick(dut); clear(dut); dut->eval();                // → IDLE
    check("post_idle", dut->o_fetch_stop, 0);

    // ── ei_shadow masks recognition until the next completion ────
    clear(dut);
    dut->i_ei_commit = 1; dut->eval(); tick(dut);      // ei_shadow <= 1
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_pipe_busy = 1; dut->eval();
    check("shadow_masks", dut->o_fetch_stop, 0);        // SR.I=1 but shadowed
    dut->i_retire_valid = 1; dut->eval(); tick(dut);    // shadow insn completes → clear
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_pipe_busy = 1; dut->eval();
    check("shadow_cleared", dut->o_fetch_stop, 1);      // now eligible

    // ── Timer outranks the external line ─────────────────────────
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_timer_irq = 1; dut->i_pipe_busy = 0;
    dut->eval(); tick(dut);                             // → DRAIN, vec latched
    dut->i_pipe_busy = 0; dut->eval();
    check("timer_priority", dut->o_irq_vec, VEC_TIMER);
    tick(dut); clear(dut); dut->eval();

    // ── A fault during the drain preempts the entry ──────────────
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_pipe_busy = 1; dut->eval(); tick(dut);  // → DRAIN
    dut->i_pipe_busy = 0; dut->i_fault_commit = 1; dut->eval();   // pipe empty but a fault commits
    check("fault_preempts", dut->o_irq_entry, 0);       // entry suppressed
    tick(dut); clear(dut); dut->eval();
    check("preempt_to_idle", dut->o_fetch_stop, 0);     // FSM returned to IDLE

    // ── A vector fetch in progress blocks recognition ────────────
    clear(dut);
    dut->i_sr_i = 1; dut->i_irq = 1; dut->i_pipe_busy = 1; dut->i_vecf_active = 1;
    dut->eval();
    check("vecf_blocks", dut->o_fetch_stop, 0);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

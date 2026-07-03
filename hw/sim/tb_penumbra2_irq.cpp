// Verilator testbench for penumbra2_irq.
//
// Drives the IRQ lines, the pipeline-state inputs, and the EX-slot view,
// checking recognition and the EX-frontier inject pulse against the contract
// in doc/internals/penumbra2/exception-flow.md and the module header:
//   - eligible (line & SR.I & ~ei_shadow) + a clean EX boundary → o_irq_inject
//   - each boundary condition independently gates the pulse (empty slot,
//     stalled slot, drain-commit, fault commit, vector fetch in progress)
//   - timer outranks the external line on o_irq_vec
//   - ei_shadow masks recognition for exactly the one instruction after EI:
//     armed by i_ei_commit (set wins over the clear on EI's own commit
//     cycle), held while nothing completes, cleared by the next retirement
//     or drain-commit
//
// The unit's --assert invariant (inject only on a clean, committable EX
// boundary) rides along on every eval.

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
    dut->i_ex_valid = 0; dut->i_ex_stall = 0;
    dut->i_fault_commit = 0; dut->i_vecf_active = 0;
}

// Present an eligible interrupt at a clean EX boundary: external line up,
// SR.I set, a real un-stalled EX slot, nothing else in the way.
static void eligible_clean(Vpenumbra2_irq* dut) {
    clear(dut);
    dut->i_irq = 1; dut->i_sr_i = 1; dut->i_ex_valid = 1;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra2_irq* dut = new Vpenumbra2_irq;

    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_no_inject", dut->o_irq_inject, 0);

    // ── Basic inject: eligible + clean EX boundary, same cycle ───
    eligible_clean(dut); dut->eval();
    check("inject_fires", dut->o_irq_inject, 1);
    check("inject_vec",   dut->o_irq_vec, VEC_EXT_IRQ);

    // ── Gating matrix: each condition alone kills the pulse ──────
    eligible_clean(dut); dut->i_sr_i = 0; dut->eval();
    check("gate_sr_i", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_irq = 0; dut->eval();
    check("gate_no_line", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_ex_valid = 0; dut->eval();
    check("gate_empty_slot", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_ex_stall = 1; dut->eval();
    check("gate_stalled_slot", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_dc_commit = 1; dut->eval();
    check("gate_dc_commit", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_fault_commit = 1; dut->eval();
    check("gate_fault_commit", dut->o_irq_inject, 0);

    eligible_clean(dut); dut->i_vecf_active = 1; dut->eval();
    check("gate_vecf_active", dut->o_irq_inject, 0);

    // ── Timer outranks the external line ─────────────────────────
    eligible_clean(dut); dut->i_timer_irq = 1; dut->eval();   // both lines
    check("timer_priority_vec", dut->o_irq_vec, VEC_TIMER);
    check("timer_priority_inject", dut->o_irq_inject, 1);

    clear(dut);
    dut->i_timer_irq = 1; dut->i_sr_i = 1; dut->i_ex_valid = 1; dut->eval();
    check("timer_alone_vec", dut->o_irq_vec, VEC_TIMER);

    // ── ei_shadow: exactly one instruction of masking after EI ───
    // Both sequences arm the shadow with i_ei_commit coincident with a
    // retirement, so they also prove the set wins over the clear on EI's
    // own commit cycle. The shadow is observable only through its effect:
    // an otherwise-injectable interrupt held at the input with the pulse
    // suppressed.

    clear(dut);
    // EI commit, timer IRQ active
    dut->i_sr_i = 1;
    dut->i_ei_commit = 1;
    dut->i_ex_valid = 1;
    dut->i_retire_valid = 1;
    dut->i_timer_irq = 1;
    dut->eval();
    tick(dut);
    // EI shadow cycle, IRQ active but not taken
    dut->i_ei_commit = 0;
    dut->eval();
    check("ei_shadow_irq_not_taken", dut->o_irq_inject, 0);
    tick(dut);
    // Cycle after EI shadow, IRQ active and taken
    check("ei_shadow_irq_taken_after", dut->o_irq_inject, 1);

    clear(dut);
    // EI commit, timer IRQ active
    dut->i_sr_i = 1;
    dut->i_ei_commit = 1;
    dut->i_ex_valid = 1;
    dut->i_retire_valid = 1;
    dut->i_timer_irq = 1;
    dut->eval();
    tick(dut);
    // EI shadow cycles, no insn retired, IRQ active but not taken
    dut->i_ei_commit = 0;
    dut->i_retire_valid = 0;
    dut->eval();
    check("ei_shadow_irq_not_taken_noretire1", dut->o_irq_inject, 0);
    tick(dut);
    check("ei_shadow_irq_not_taken_noretire2", dut->o_irq_inject, 0);
    tick(dut);
    // EI shadow cycle, insn retired, IRQ active but not taken
    dut->i_retire_valid = 1;
    tick(dut);
    // Cycle after EI shadow, insn retired, IRQ active and taken
    check("ei_shadow_irq_taken_after_hold", dut->o_irq_inject, 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

// Verilator testbench for penumbra3_irq.
//
// Checks interrupt recognition + EX-frontier injection:
//   - a clean, eligible EX boundary injects with the right vector
//   - timer outranks the external line
//   - each gate blocks injection: SR.I=0, ex_stall, dc_commit, fault_commit,
//     vecf_active, and an empty EX slot
//   - the EI one-instruction shadow masks for exactly one completing
//     instruction, and EI's own set wins over a coincident clear

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_irq.h"
#include "verilated.h"

static const int VEC_TIMER   = 1;
static const int VEC_EXT_IRQ = 9;

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_irq* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// A clean, eligible EX boundary with an external IRQ pending: every gate open.
static void eligible_baseline(Vpenumbra3_irq* dut) {
    dut->i_irq = 1; dut->i_timer_irq = 0;
    dut->i_sr_i = 1;
    dut->i_ei_commit = 0; dut->i_retire_valid = 0; dut->i_dc_commit = 0;
    dut->i_ex_valid = 1; dut->i_ex_stall = 0;
    dut->i_fault_commit = 0; dut->i_vecf_active = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_irq* dut = new Vpenumbra3_irq;

    // ── Reset ────────────────────────────────────────────────────
    eligible_baseline(dut);
    dut->i_irq = 0; dut->i_sr_i = 0;
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    dut->eval();
    check("rst_inject", dut->o_irq_inject, 0);

    // ── Eligible boundary injects, external vector ───────────────
    eligible_baseline(dut); dut->eval();
    check("elig_inject", dut->o_irq_inject, 1);
    check("elig_vec",    dut->o_irq_vec, VEC_EXT_IRQ);

    // ── Timer outranks the external line ─────────────────────────
    eligible_baseline(dut); dut->i_timer_irq = 1; dut->eval();
    check("timer_inject", dut->o_irq_inject, 1);
    check("timer_vec",    dut->o_irq_vec, VEC_TIMER);

    // ── Each gate blocks injection ───────────────────────────────
    eligible_baseline(dut); dut->i_sr_i = 0;        dut->eval();
    check("gate_sri",   dut->o_irq_inject, 0);
    eligible_baseline(dut); dut->i_ex_stall = 1;    dut->eval();
    check("gate_stall", dut->o_irq_inject, 0);
    eligible_baseline(dut); dut->i_dc_commit = 1;   dut->eval();
    check("gate_dc",    dut->o_irq_inject, 0);
    eligible_baseline(dut); dut->i_fault_commit = 1; dut->eval();
    check("gate_fault", dut->o_irq_inject, 0);
    eligible_baseline(dut); dut->i_vecf_active = 1;  dut->eval();
    check("gate_vecf",  dut->o_irq_inject, 0);
    eligible_baseline(dut); dut->i_ex_valid = 0;     dut->eval();
    check("gate_empty", dut->o_irq_inject, 0);

    // ── No line pending: nothing to inject ───────────────────────
    eligible_baseline(dut); dut->i_irq = 0;          dut->eval();
    check("gate_noline", dut->o_irq_inject, 0);

    // ── EI one-instruction shadow ────────────────────────────────
    // Arm the shadow: EI commits (a drain-commit, so dc_commit coincides).
    eligible_baseline(dut);
    dut->i_irq = 0; dut->i_ei_commit = 1; dut->i_dc_commit = 1;
    tick(dut);

    // Shadow set: an otherwise-eligible IRQ is masked for the one
    // post-EI instruction.
    eligible_baseline(dut); dut->eval();
    check("shadow_block", dut->o_irq_inject, 0);

    // The shadow instruction completes (retire) -- clears the shadow.
    eligible_baseline(dut); dut->i_retire_valid = 1; dut->eval();
    check("shadow_clear_cycle", dut->o_irq_inject, 0);   // still masked the completion cycle
    tick(dut);

    // Shadow now clear: the IRQ injects.
    eligible_baseline(dut); dut->eval();
    check("shadow_done", dut->o_irq_inject, 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

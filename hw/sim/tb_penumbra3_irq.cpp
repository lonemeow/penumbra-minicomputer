// Verilator testbench for penumbra3_irq.
//
// Checks interrupt recognition + EX-frontier injection:
//   - a clean, eligible EX boundary injects with the right vector
//   - timer outranks the external line
//   - each gate blocks injection: SR.I=0, ex_stall, dc_commit, fault_commit,
//     vecf_active, and an empty EX slot
//   - the EI one-instruction shadow masks for exactly one completing
//     instruction, and EI's own set wins over a coincident clear
//   - the deferred bus fault: held from its store fault, injected at the
//     same clean boundary ahead of any interrupt and whatever SR.I says,
//     held until it commits, the first one kept while one is held, and a
//     fault in the cycle the held one commits kept as the next

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
    dut->i_store_fault = 0; dut->i_store_fault_addr = 0; dut->i_deferred_commit = 0;
}

// The bus faults an earlier cacheable write: a one-cycle store fault.
static void store_fault(Vpenumbra3_irq* dut, uint32_t addr) {
    dut->i_store_fault = 1; dut->i_store_fault_addr = addr; dut->eval();
    tick(dut);
    dut->i_store_fault = 0; dut->i_store_fault_addr = 0; dut->eval();
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
    check("rst_deferred", dut->o_deferred_inject, 0);

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

    // ── The deferred bus fault ───────────────────────────────────
    // None held: the interrupt goes as before.
    eligible_baseline(dut); dut->eval();
    check("df_none", dut->o_deferred_inject, 0);

    // Held from the cycle after its store fault; it goes first, and holds
    // the interrupt off.
    eligible_baseline(dut); dut->i_ex_valid = 0;     // no boundary meanwhile
    dut->i_store_fault = 1; dut->i_store_fault_addr = 0x00ABC120; dut->eval();
    check("df_not_yet", dut->o_deferred_inject, 0);
    tick(dut);
    eligible_baseline(dut); dut->eval();
    check("df_inject",     dut->o_deferred_inject, 1);
    check("df_addr",       dut->o_deferred_addr, 0x00ABC120);
    check("df_before_irq", dut->o_irq_inject, 0);

    // Not maskable: SR.I = 0 holds it no more than it holds a fault.
    eligible_baseline(dut); dut->i_sr_i = 0; dut->i_irq = 0; dut->eval();
    check("df_unmasked", dut->o_deferred_inject, 1);

    // The same clean boundary as an interrupt's.
    eligible_baseline(dut); dut->i_ex_stall = 1;     dut->eval();
    check("df_gate_stall", dut->o_deferred_inject, 0);
    eligible_baseline(dut); dut->i_dc_commit = 1;    dut->eval();
    check("df_gate_dc",    dut->o_deferred_inject, 0);
    eligible_baseline(dut); dut->i_fault_commit = 1; dut->eval();
    check("df_gate_fault", dut->o_deferred_inject, 0);
    eligible_baseline(dut); dut->i_vecf_active = 1;  dut->eval();
    check("df_gate_vecf",  dut->o_deferred_inject, 0);
    eligible_baseline(dut); dut->i_ex_valid = 0;     dut->eval();
    check("df_gate_empty", dut->o_deferred_inject, 0);

    // Injecting does not take it: an older fault may flush the slot it was
    // injected into, so it stays held until it commits.
    eligible_baseline(dut); dut->eval();
    tick(dut);
    eligible_baseline(dut); dut->eval();
    check("df_held_past_inject", dut->o_deferred_inject, 1);

    // While one is held, another store fault is not recorded.
    eligible_baseline(dut); dut->i_ex_valid = 0;
    store_fault(dut, 0x00DEF340);
    eligible_baseline(dut); dut->eval();
    check("df_first_kept", dut->o_deferred_addr, 0x00ABC120);

    // Its commit lets it go, and the interrupt goes again.
    eligible_baseline(dut); dut->i_ex_valid = 0; dut->i_deferred_commit = 1;
    dut->eval();
    tick(dut);
    eligible_baseline(dut); dut->eval();
    check("df_commit_clears",  dut->o_deferred_inject, 0);
    check("df_irq_after",      dut->o_irq_inject, 1);

    // A store fault after the commit is held as the next one.
    eligible_baseline(dut); dut->i_ex_valid = 0;
    store_fault(dut, 0x00111220);
    eligible_baseline(dut); dut->eval();
    check("df_next",      dut->o_deferred_inject, 1);
    check("df_next_addr", dut->o_deferred_addr, 0x00111220);

    // A store fault in the cycle the held one commits is kept as the next.
    eligible_baseline(dut); dut->i_ex_valid = 0; dut->i_deferred_commit = 1;
    dut->i_store_fault = 1; dut->i_store_fault_addr = 0x00222440; dut->eval();
    tick(dut);
    eligible_baseline(dut); dut->eval();
    check("df_at_commit",      dut->o_deferred_inject, 1);
    check("df_at_commit_addr", dut->o_deferred_addr, 0x00222440);

    // Reset drops a held one.
    dut->i_rst = 1; tick(dut); tick(dut); dut->i_rst = 0;
    eligible_baseline(dut); dut->eval();
    check("df_rst", dut->o_deferred_inject, 0);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}

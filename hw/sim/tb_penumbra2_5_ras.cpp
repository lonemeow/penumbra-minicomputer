// Verilator testbench for the Penumbra/2.5 penumbra2_ras return-address stack.
//
// Tests the leaf to its contract — the stack mechanism itself, independent of
// the front end that feeds it (penumbra2_id_stage). This matters because the
// RAS is a *performance* structure guarded by EX: the full-machine suite can't
// tell a working RAS from a dead one (execution is correct either way, since EX
// corrects every misprediction). So the stack must be observed directly here.
//
// Coverage:
//   - basic LIFO order (push A,B,C → pop C,B,A → empty),
//   - exactly full (push DEPTH): the count-saturation boundary — every entry
//     survives, nothing overwritten,
//   - overflow by one wrap (DEPTH+2) and past a full revolution (2*DEPTH+1):
//     the oldest are overwritten, only the newest DEPTH survive newest-first —
//     the ring discipline checked on both sides of a full pointer revolution,
//   - underflow: repeated pops on an empty stack are no-ops (count floors at 0,
//     top does not wander) and the stack still works afterward.
//
// o_target is combinational (the current top), so it is read *before* the pop
// that consumes it — mirroring how a return reads its predicted target the same
// cycle it pops. DEPTH here tracks the module's default parameter.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_ras.h"

static const int DEPTH = 8;   // == penumbra2_ras default DEPTH

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Vpenumbra2_ras* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}
static void reset(Vpenumbra2_ras* dut) {
    dut->i_push = 0; dut->i_pop = 0; dut->i_link_addr = 0;
    dut->i_rst = 1; tick(dut); tick(dut);   // held 2 cycles, per convention
    dut->i_rst = 0; dut->eval();
}
static void push(Vpenumbra2_ras* dut, uint32_t addr) {
    dut->i_push = 1; dut->i_pop = 0; dut->i_link_addr = addr;
    tick(dut);
    dut->i_push = 0; dut->eval();           // settle combinational outputs on the new top
}
static void pop(Vpenumbra2_ras* dut) {
    dut->i_pop = 1; dut->i_push = 0;
    tick(dut);
    dut->i_pop = 0; dut->eval();
}

int main() {
    Vpenumbra2_ras* dut = new Vpenumbra2_ras;

    // ── Reset: an empty stack predicts nothing ───────────────────
    reset(dut);
    check("reset.valid", dut->o_valid, 0);

    // ── Basic LIFO: push A,B,C; pop returns C,B,A, then empty ────
    push(dut, 0xAAAA0000);
    check("pushA.valid", dut->o_valid, 1);
    check("pushA.top",   dut->o_target, 0xAAAA0000);
    push(dut, 0xBBBB0000);
    check("pushB.top",   dut->o_target, 0xBBBB0000);
    push(dut, 0xCCCC0000);
    check("pushC.top",   dut->o_target, 0xCCCC0000);
    // Each pop consumes the value visible now (newest first).
    check("lifo.C", dut->o_target, 0xCCCC0000); pop(dut);
    check("lifo.B", dut->o_target, 0xBBBB0000); pop(dut);
    check("lifo.A", dut->o_target, 0xAAAA0000); pop(dut);
    check("lifo.empty", dut->o_valid, 0);

    // ── Exactly full: push DEPTH; every entry survives (no overwrite) ──
    // The count-saturation boundary — count first reaches DEPTH but nothing is
    // overwritten yet, so all DEPTH must pop back newest-first.
    reset(dut);
    for (int i = 1; i <= DEPTH; i++)
        push(dut, 0x100 * i);               // links 0x100 .. 0x(DEPTH)00
    for (int v = DEPTH; v >= 1; v--) {
        check("full.valid", dut->o_valid, 1);
        check("full.top",   dut->o_target, (uint32_t)(0x100 * v));
        pop(dut);
    }
    check("full.drained", dut->o_valid, 0);

    // ── Overflow by 2: one wrap; the two oldest are overwritten ──
    reset(dut);
    for (int i = 1; i <= DEPTH + 2; i++)
        push(dut, 0x1000 * i);              // links 0x1000 .. 0x(DEPTH+2)000
    // 0x1000, 0x2000 overwritten; the newest DEPTH pop newest-first.
    for (int v = DEPTH + 2; v >= 3; v--) {
        check("ovf2.valid", dut->o_valid, 1);
        check("ovf2.top",   dut->o_target, (uint32_t)(0x1000 * v));
        pop(dut);
    }
    check("ovf2.drained", dut->o_valid, 0);

    // ── Overflow past a full revolution: 2*DEPTH+1 pushes ────────
    // top wraps the ring more than once — catches wrap arithmetic that a single
    // overflow step (+2) leaves on the happy side of the boundary. Only the
    // newest DEPTH survive: pushes n .. n-DEPTH+1.
    reset(dut);
    int n = 2 * DEPTH + 1;
    for (int i = 1; i <= n; i++)
        push(dut, 0x10 * i);
    for (int v = n; v >= n - DEPTH + 1; v--) {
        check("ovfwrap.valid", dut->o_valid, 1);
        check("ovfwrap.top",   dut->o_target, (uint32_t)(0x10 * v));
        pop(dut);
    }
    check("ovfwrap.drained", dut->o_valid, 0);

    // ── Underflow: repeated pops on an empty stack are no-ops ─────
    // count must floor at 0 (never wrap to a huge value, which would make
    // o_valid lie), and top must not wander — the stack still works afterward.
    reset(dut);
    for (int k = 0; k < 3; k++) {
        pop(dut);
        check("underflow.valid", dut->o_valid, 0);
    }
    push(dut, 0x5A5A0000);                  // heals cleanly after the underflow
    check("heal.valid", dut->o_valid, 1);
    check("heal.top",   dut->o_target, 0x5A5A0000);
    pop(dut);
    check("heal.empty", dut->o_valid, 0);

    if (errors == 0) printf("penumbra2_ras: all %d checks passed\n", tests);
    else             printf("penumbra2_ras: %d/%d checks FAILED\n", errors, tests);
    delete dut;
    return errors != 0;
}

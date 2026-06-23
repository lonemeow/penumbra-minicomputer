// Verilator testbench for the Penumbra/2.5 penumbra2_btb branch target buffer.
//
// Tests the leaf to its contract — the tagged target cache itself, independent
// of the front end that feeds it. Like the RAS, the BTB is a *performance*
// structure guarded by EX: the full-machine suite cannot tell a working BTB
// from a dead one (execution is correct either way, since EX corrects every
// misprediction), so the cache behaviour must be observed directly here.
//
// Timing mirrors the hardware: the read is registered, launched on a lookup
// cycle (i_lookup_en) and consumed the next, so o_hit / o_target describe the
// PC looked up the previous cycle. Updates carry a resolved branch's PC,
// target, and direction.
//
// Coverage:
//   - reset predicts nothing,
//   - allocate-on-taken then hit, returning the cached target,
//   - tag disambiguation: an aliasing PC (same index, different tag) misses,
//   - refresh: re-allocating a resident PC updates its target,
//   - invalidate-on-not-taken: a not-taken resolve drops the entry,
//   - tag-checked invalidate: a not-taken *aliasing* PC spares a resident,
//     tag-mismatched neighbour (the property that distinguishes a tag-checked
//     invalidate from an unconditional one),
//   - lockstep freeze: a disabled lookup holds the previous result, so the
//     prediction stays matched to a stalled, held icache word.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_btb.h"

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Vpenumbra2_btb* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}
static void reset(Vpenumbra2_btb* dut) {
    dut->i_lookup_en = 0; dut->i_lookup_pc = 0;
    dut->i_update = 0; dut->i_update_pc = 0;
    dut->i_update_target = 0; dut->i_update_taken = 0;
    dut->i_rst = 1; tick(dut); tick(dut);   // held 2 cycles, per convention
    dut->i_rst = 0; dut->eval();
}
// One update cycle, no concurrent lookup: allocate (taken) or invalidate.
static void update(Vpenumbra2_btb* dut, uint32_t pc, uint32_t target, bool taken) {
    dut->i_lookup_en = 0;
    dut->i_update = 1; dut->i_update_pc = pc;
    dut->i_update_target = target; dut->i_update_taken = taken;
    tick(dut);
    dut->i_update = 0; dut->eval();
}
// Launch a lookup; the registered result settles after the tick, read off the
// combinational outputs (independent of i_lookup_en once captured).
static void lookup(Vpenumbra2_btb* dut, uint32_t pc) {
    dut->i_update = 0;
    dut->i_lookup_en = 1; dut->i_lookup_pc = pc;
    tick(dut);
    dut->i_lookup_en = 0; dut->eval();
}

int main() {
    Vpenumbra2_btb* dut = new Vpenumbra2_btb;

    // ENTRIES=32 default → index = PC[6:2]. Two PCs 0x80 apart share an index
    // but differ in tag.
    const uint32_t P      = 0x00001000;
    const uint32_t PALIAS = 0x00001080;
    const uint32_t T      = 0x00002000;
    const uint32_t T2     = 0x00003000;

    // ── Reset: nothing is predicted ──────────────────────────────
    reset(dut);
    lookup(dut, P);
    check("reset.hit", dut->o_hit, 0);

    // ── Allocate-on-taken then hit, with the cached target ───────
    update(dut, P, T, /*taken=*/true);
    lookup(dut, P);
    check("alloc.hit",    dut->o_hit, 1);
    check("alloc.target", dut->o_target, T);

    // ── Tag disambiguation: an aliasing PC misses ────────────────
    lookup(dut, PALIAS);
    check("alias.miss", dut->o_hit, 0);

    // ── Refresh: re-allocating the same PC updates its target ────
    update(dut, P, T2, true);
    lookup(dut, P);
    check("refresh.hit",    dut->o_hit, 1);
    check("refresh.target", dut->o_target, T2);

    // ── Invalidate-on-not-taken: the entry is dropped ────────────
    update(dut, P, 0, /*taken=*/false);
    lookup(dut, P);
    check("inval.miss", dut->o_hit, 0);

    // ── Tag-checked invalidate: a not-taken alias spares a resident
    //    tag-mismatched neighbour ──────────────────────────────────
    update(dut, P, T, true);          // P resident again
    update(dut, PALIAS, 0, false);    // not-taken on the aliasing PC
    lookup(dut, P);
    check("alias-inval.hit",    dut->o_hit, 1);   // P survived the alias's invalidate
    check("alias-inval.target", dut->o_target, T);

    // ── Lockstep freeze: a disabled lookup holds the last result ─
    lookup(dut, P);                                   // P hit captured
    dut->i_lookup_en = 0; dut->i_lookup_pc = PALIAS;  // would miss if the read advanced
    tick(dut); dut->eval();
    check("freeze.hit",    dut->o_hit, 1);
    check("freeze.target", dut->o_target, T);

    if (errors == 0) printf("penumbra2_btb: all %d checks passed\n", tests);
    else             printf("penumbra2_btb: %d/%d checks FAILED\n", errors, tests);
    delete dut;
    return errors != 0;
}

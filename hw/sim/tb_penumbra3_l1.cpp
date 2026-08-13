// Verilator testbench for penumbra3_l1 (gen3 passive VIPT L1).
//
// The module has no back side to model -- it raises no request and owns no
// FSM -- so the testbench plays every role the completion unit and the fill
// sequencer will: it launches lookups, presents the resolve paddr, streams
// fill beats, and drives the sysreg port directly.
//
// Timing follows the module's launch/resolve contract: i_vaddr is sampled at
// the edge when (i_en & ~i_hold); the verdict for that access is
// combinational one cycle later against i_paddr. The deferred PLRU touch
// lands two edges after a resolve.
//
// Covers:
//   - reset: all ways invalid, cache disabled
//   - INFO geometry readback and CTRL enable readback / o_enabled
//   - disabled: no hit, no store absorbed, and the whole fill is inert --
//     a fill attempted while disabled must not scribble the tag of a line
//     that survived from before the disable
//   - miss -> fill -> install -> hit, per word of the line
//   - store hit updates the local copy under its byte enables; store miss
//     allocates nothing (write-no-allocate)
//   - associativity: both ways of a set resident at once
//   - victim selection: invalid-first in index order, then tree-PLRU
//   - INVAL_ALL drops every line
//   - VIPT: the tag compare is physical, so equal vaddr index bits with
//     different paddr tags are different lines
//   - i_hold: the freeze holds the resolved verdict (see test_freeze)
//
// The module's own SVA assertions run under --assert and police the launch /
// fill exclusion and the VIPT precondition throughout.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_l1.h"
#include "verilated.h"

// Geometry -- must match the module's default parameters.
static const int      LINE_WORDS = 4;
static const int      NUM_WAYS   = 2;
static const int      NUM_SETS   = 256;
static const uint32_t LINE_MASK  = 0xFu;      // LINE_BYTES - 1

// Unified cache sysreg map (penumbra_pkg.sv).
static const int REG_INFO = 0, REG_CTRL = 1, REG_INVAL_ALL = 2;
static const int CACHE_ADDR_VIPT = 1;

static Vpenumbra3_l1* d;
static int errors = 0, tests = 0;

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp);
        errors++;
    }
}

static void tick() { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

// Drop the launch controls and drain one edge before clearing i_paddr: a
// lookup already in flight still owes the module a matching resolve paddr,
// and the VIPT assertion is entitled to hold us to it.
static void idle() {
    d->i_en = 0; d->i_hold = 0;
    d->i_store_we = 0; d->i_fill_we = 0; d->i_fill_done = 0; d->i_sys_we = 0;
    d->eval(); tick();
    d->i_vaddr = 0; d->i_paddr = 0;
    d->i_store_wdata = 0; d->i_store_byte_en = 0;
    d->i_fill_paddr = 0; d->i_fill_way = 0;
    d->i_fill_word = 0; d->i_fill_wdata = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0;
    d->eval();
}

// ── Sysreg port ─────────────────────────────────────────────────
// INVAL_ALL is a registered request, so the flash-clear lands one edge after
// the write edge; the trailing tick covers it for every register uniformly.
static void sys_write(int reg, uint32_t val) {
    d->i_sys_reg = reg; d->i_sys_wdata = val; d->i_sys_we = 1;
    d->eval(); tick();
    d->i_sys_we = 0; d->eval(); tick();
}

static uint32_t sys_read(int reg) {
    d->i_sys_reg = reg; d->eval();
    return d->o_sys_rdata;
}

static void set_enable(bool on) { sys_write(REG_CTRL, on ? 1 : 0); }

// ── Front side ──────────────────────────────────────────────────
// Launch at vaddr, resolve against paddr, and leave the DUT parked on the
// resolve cycle so the caller can read o_hit / o_rdata / o_victim_way.
static bool probe(uint32_t vaddr, uint32_t paddr, uint32_t* rdata = nullptr) {
    d->i_en = 1; d->i_hold = 0; d->i_vaddr = vaddr;
    d->eval(); tick();
    d->i_en = 0; d->i_paddr = paddr; d->eval();
    if (rdata) *rdata = d->o_rdata;
    return d->o_hit != 0;
}

// Two edges past a resolve the deferred PLRU touch has landed.
static void settle() { tick(); tick(); d->eval(); }

// A store resolves like a load and drives the write port on the resolve
// cycle; returns the hit verdict the write was qualified by.
static bool store(uint32_t vaddr, uint32_t paddr, uint32_t data, int byte_en) {
    d->i_en = 1; d->i_hold = 0; d->i_vaddr = vaddr;
    d->eval(); tick();
    d->i_en = 0; d->i_paddr = paddr;
    d->i_store_we = 1; d->i_store_wdata = data; d->i_store_byte_en = byte_en;
    d->eval();
    bool hit = d->o_hit != 0;
    tick();
    d->i_store_we = 0; d->i_store_byte_en = 0; d->eval();
    settle();
    return hit;
}

// Stream a line into a way, installing on the last beat (the shape the fill
// sequencer drives). The pipe is frozen for the duration, as the module's
// launch/fill exclusion assertion requires.
static void fill_line(uint32_t paddr, int way, const uint32_t* words) {
    d->i_en = 0; d->i_hold = 0;
    d->i_fill_paddr = paddr & ~LINE_MASK;
    d->i_fill_way = way;
    for (int i = 0; i < LINE_WORDS; i++) {
        d->i_fill_we = 1;
        d->i_fill_word = i;
        d->i_fill_wdata = words[i];
        d->i_fill_done = (i == LINE_WORDS - 1);
        d->eval(); tick();
    }
    d->i_fill_we = 0; d->i_fill_done = 0; d->eval();
    settle();
}

// ── Test addresses ──────────────────────────────────────────────
// All three lines index set 5 (vaddr[11:4]) and differ only in the physical
// tag, so they collide in the cache while satisfying VIPT (vaddr and paddr
// agree on bits [11:0]).
static const uint32_t VA_A = 0x00000050, PA_A = 0x00010050;
static const uint32_t VA_B = 0x00002050, PA_B = 0x00020050;
static const uint32_t VA_C = 0x00004050, PA_C = 0x00030050;

static const uint32_t LINE_A[LINE_WORDS] = {0xA0000000, 0xA1111111, 0xA2222222, 0xA3333333};
static const uint32_t LINE_B[LINE_WORDS] = {0xB0000000, 0xB1111111, 0xB2222222, 0xB3333333};
static const uint32_t LINE_C[LINE_WORDS] = {0xC0000000, 0xC1111111, 0xC2222222, 0xC3333333};

// Word 0 of line A after the store-hit test merges 0xBEEF into its low half;
// every later read of that word expects the merged value.
static const uint32_t A_W0_STORED = (LINE_A[0] & 0xFFFF0000) | 0xBEEF;

// ── The freeze convention ───────────────────────────────────────
// i_en means "a real access is in the launch stage" and i_hold is the pipe
// freeze; only their conjunction may sample. The launch stage can hold a
// valid access across a freeze, so i_en alone stays high while the pipe is
// stalled -- the module must not treat that as a new lookup, or the verdict
// belonging to the access held in the resolve stage is overwritten by the
// address of the one held behind it.
static void test_freeze() {
    // Line A is resident in way 0 and line B in way 1 (set 5) on entry;
    // probing A leaves the DUT parked on A's resolve cycle.
    uint32_t rdata = 0;
    check("frz_setup_hit", probe(VA_A, PA_A, &rdata), 1);
    check("frz_setup_rdata", rdata, A_W0_STORED);

    // TODO(human): pin the freeze behaviour from this parked state.
    //
    // Drive i_hold high with i_en high and i_vaddr pointing somewhere else
    // (VA_B is resident and would resolve differently; a set the cache has
    // never seen works too), tick a couple of times, and check that the
    // verdict still describes A -- o_hit and o_rdata unchanged against
    // PA_A. Then drop i_hold and confirm a launch takes effect again, so
    // the freeze holds the lookup rather than cancelling it.
    //
    // Use check() for each assertion so the counts roll up with the rest,
    // and leave the DUT idle (i_en = i_hold = 0) when you return.
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Vpenumbra3_l1;

    idle();
    d->i_rst = 1; tick(); tick(); d->i_rst = 0; d->eval();

    // ── Reset state ──
    check("rst_enabled", d->o_enabled, 0);              // disabled at reset
    check("rst_miss", probe(VA_A, PA_A), 0);            // nothing valid

    // ── Sysreg: INFO geometry, CTRL enable ──
    idle();
    check("info", sys_read(REG_INFO),
          ((uint32_t)CACHE_ADDR_VIPT << 26) | ((uint32_t)NUM_WAYS << 21)
          | ((uint32_t)NUM_SETS << 6) | (uint32_t)LINE_WORDS);
    set_enable(true);
    check("ctrl_rb", sys_read(REG_CTRL), 1);
    check("enabled_out", d->o_enabled, 1);

    // ── Miss -> fill -> hit, every word of the line ──
    idle();
    check("a_miss", probe(VA_A, PA_A), 0);
    check("a_victim", d->o_victim_way, 0);              // invalid-first: way 0
    settle();
    fill_line(PA_A, 0, LINE_A);
    for (int w = 0; w < LINE_WORDS; w++) {
        uint32_t rd = 0;
        char n[32]; snprintf(n, sizeof n, "a_hit_w%d", w);
        check(n, probe(VA_A + 4 * w, PA_A + 4 * w, &rd), 1);
        snprintf(n, sizeof n, "a_rdata_w%d", w);
        check(n, rd, LINE_A[w]);
        settle();
    }

    // ── VIPT: same index bits, different physical tag -> different line ──
    idle();
    check("b_miss_vipt", probe(VA_B, PA_B), 0);
    check("b_victim", d->o_victim_way, 1);              // way 0 taken, way 1 free
    settle();

    // ── Associativity: both ways of set 5 resident at once ──
    fill_line(PA_B, 1, LINE_B);
    check("a_still_hit", probe(VA_A, PA_A), 1);
    settle();
    uint32_t rd_b = 0;
    check("b_hit", probe(VA_B, PA_B, &rd_b), 1);
    check("b_rdata", rd_b, LINE_B[0]);
    settle();

    // ── Store hit: byte enables update the local copy ──
    idle();
    check("st_hit", store(VA_A, PA_A, 0xDEADBEEF, 0x3), 1);   // low half only
    uint32_t rd = 0;
    check("st_read_back", probe(VA_A, PA_A, &rd), 1);
    check("st_merged", rd, A_W0_STORED);
    settle();

    // ── Store miss: write-no-allocate, nothing installed ──
    idle();
    check("stm_miss", store(VA_C, PA_C, 0x5A5A5A5A, 0xF), 0);
    check("stm_still_miss", probe(VA_C, PA_C), 0);
    settle();

    // ── Victim selection: full set consults the PLRU tree ──
    // Filling way 1 last left way 0 as LRU; touching A (way 0) on the hit
    // above flipped it, so the victim for set 5 is now way 1.
    idle();
    check("plru_victim", probe(VA_C, PA_C), 0);
    check("plru_victim_way", d->o_victim_way, 1);
    settle();
    check("plru_after_b", probe(VA_B, PA_B), 1);        // touch way 1
    settle();
    check("plru_victim2", probe(VA_C, PA_C), 0);
    check("plru_victim_way2", d->o_victim_way, 0);      // ... victim swings back
    settle();

    // ── The freeze convention (see the note above test_freeze) ──
    idle();
    test_freeze();

    // ── Disabled: no hit, no store absorbed, fill inert ──
    idle();
    set_enable(false);
    check("dis_enabled_out", d->o_enabled, 0);
    check("dis_no_hit", probe(VA_A, PA_A), 0);          // resident, but passes through
    settle();
    check("dis_store_miss", store(VA_A, PA_A, 0x11111111, 0xF), 0);

    // A fill attempted while disabled must not touch the tag array: line A
    // still holds a valid way, and a scribbled tag would read back as a hit
    // on line C's data once software re-enables.
    fill_line(PA_C, 0, LINE_C);
    set_enable(true);
    uint32_t rd_a = 0;
    check("reen_a_hit", probe(VA_A, PA_A, &rd_a), 1);
    check("reen_a_intact", rd_a, A_W0_STORED);
    settle();
    check("reen_c_absent", probe(VA_C, PA_C), 0);
    settle();

    // ── INVAL_ALL drops every line ──
    idle();
    sys_write(REG_INVAL_ALL, 0);
    check("inval_a", probe(VA_A, PA_A), 0);
    settle();
    check("inval_b", probe(VA_B, PA_B), 0);
    check("inval_victim", d->o_victim_way, 0);          // empty set: way 0 again
    settle();

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}

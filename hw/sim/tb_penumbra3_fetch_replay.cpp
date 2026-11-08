// Verilator testbench for penumbra3_fetch_replay.
//
// Plays IF2 and the L1-I's access around the unit, one contract clause per
// group: the replay and its PC, the delivery to the replayed slot and how
// long it lasts, a flush before, at and after the done, and a done that is
// no replay's (a killed fetch's, or the vector read's).
//
// Timing: the miss in cycle m makes o_replay high in m+1; the L1-I is busy
// from m+1; a done in d makes o_deliver high from d+1 until the slot leaves
// IF2 or a flush.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_fetch_replay.h"
#include "verilated.h"

typedef Vpenumbra3_fetch_replay Dut;

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Dut* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void clear(Dut* d) {
    d->i_miss = 0; d->i_miss_pc = 0;
    d->i_busy = 0; d->i_access_done = 0;
    d->i_flush = 0; d->i_slot_left = 0;
    d->eval();
}

// The miss cycle m: the L1-I takes the access and is busy from m+1. Leaves
// the DUT in m+1, the next fetch's PC on IF2's port.
static void miss(Dut* d, uint32_t pc) {
    d->i_miss = 1; d->i_miss_pc = pc; d->eval();
    tick(d);
    d->i_miss = 0; d->i_miss_pc = pc + 4; d->i_busy = 1; d->eval();
}

// The L1-I's done in this cycle, which ends its busy; leaves the DUT in the
// cycle after.
static void done(Dut* d) {
    d->i_busy = 0; d->i_access_done = 1; d->eval();
    tick(d);
    d->i_access_done = 0; d->eval();
}

// A slot leaves IF2 in this cycle.
static void slot_left(Dut* d) {
    d->i_slot_left = 1; d->eval();
    tick(d);
    d->i_slot_left = 0; d->eval();
}

static void flush(Dut* d) {
    d->i_flush = 1; d->eval();
    tick(d);
    d->i_flush = 0; d->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;

    clear(d);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();
    check("rst_replay",   d->o_replay, 0);
    check("rst_replayed", d->o_replayed, 0);
    check("rst_deliver",  d->o_deliver, 0);

    // ── The replay: the cycle after the miss, to the missed PC ───
    miss(d, 0x1000);                                         // m+1
    check("replay",        d->o_replay, 1);
    check("replay_pc",     d->o_replay_pc, 0x1000);
    check("replayed",      d->o_replayed, 1);
    check("no_deliver_m1", d->o_deliver, 0);
    tick(d);                                                 // m+2
    check("replay_one_cycle", d->o_replay, 0);
    check("replay_pc_held",   d->o_replay_pc, 0x1000);       // busy: the PC stays
    check("no_deliver_busy",  d->o_deliver, 0);

    // ── The delivery: from the cycle after the done to the slot leaving ─
    done(d);                                                 // d+1
    check("deliver",          d->o_deliver, 1);
    check("deliver_replayed", d->o_replayed, 1);
    tick(d);                                                 // the slot waits in IF2
    check("deliver_held",     d->o_deliver, 1);
    slot_left(d);
    check("deliver_dropped",  d->o_deliver, 0);
    check("replayed_dropped", d->o_replayed, 0);
    tick(d);
    check("deliver_once",     d->o_deliver, 0);

    // ── A flush before the done: nothing delivered ───────────────
    miss(d, 0x2000);
    tick(d);
    flush(d);                                                // a redirect
    check("flush_replayed", d->o_replayed, 0);
    tick(d);
    done(d);                                                 // the killed fetch's done
    check("flush_no_deliver", d->o_deliver, 0);
    tick(d);
    check("flush_no_deliver_later", d->o_deliver, 0);

    // ── A flush in the done's cycle wins ─────────────────────────
    miss(d, 0x3000);
    tick(d);
    d->i_flush = 1;
    done(d);
    d->i_flush = 0; d->eval();
    check("flush_at_done_no_deliver", d->o_deliver, 0);
    check("flush_at_done_replayed",   d->o_replayed, 0);

    // ── A flush after the done takes the delivery back ───────────
    miss(d, 0x4000);
    tick(d);
    done(d);
    check("late_flush_deliver", d->o_deliver, 1);
    flush(d);
    check("late_flush_dropped", d->o_deliver, 0);
    check("late_flush_replayed", d->o_replayed, 0);

    // ── A done that is no replay's: the vector read ──────────────
    d->i_busy = 1; d->eval();
    tick(d); tick(d);
    done(d);
    check("vec_no_deliver", d->o_deliver, 0);
    check("vec_no_replay",  d->o_replay, 0);

    // ── A miss right behind a delivery: its own PC, its own word ─
    miss(d, 0x5000);
    tick(d);
    done(d);
    check("quick_deliver", d->o_deliver, 1);
    slot_left(d);
    miss(d, 0x5004);
    check("next_replay_pc", d->o_replay_pc, 0x5004);
    check("next_no_deliver_before_done", d->o_deliver, 0);
    tick(d);
    done(d);
    check("next_deliver", d->o_deliver, 1);
    slot_left(d);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}

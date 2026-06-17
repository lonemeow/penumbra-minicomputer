// Verilator testbench for penumbra2_fetch_buffer.
//
// The fetch buffer is the elastic decoupling FIFO at the IF2 -> ID seam; its
// whole purpose is to break the back-end stall path. This checks its contract:
//   - o_enq_ready depends only on occupancy (deasserts at full), never on
//     i_deq_ready; o_deq_valid == not-empty.
//   - 1-cycle latency: a word enqueued into an empty FIFO appears at the head
//     the *next* cycle, not the same cycle.
//   - FIFO order + data integrity under every handshake combination, with
//     special attention to simultaneous enqueue+dequeue (the classic FIFO
//     corruption corner) at occupancy 1 and at full.
//   - full rejects a coincident enqueue (the one-cycle out-of-full bubble).
//   - i_flush empties in one cycle and overrides a coincident enqueue/dequeue.
//
// A randomized reference-model soak (golden std::deque) is the safety net; the
// directed corners are named regressions on top of it. The payload is 133 bits
// (a VlWide of 5 words); each tag is expanded to a per-word signature so any
// wrong-slot read or partial corruption shows up, not just a wrong low word.

#include <cstdio>
#include <cstdint>
#include <deque>
#include "Vpenumbra2_fetch_buffer.h"

static const int      DEPTH   = 2;          // module default
static const int      NWORDS  = 5;          // ceil(133/32)
static const uint32_t W4_MASK = 0x1Fu;      // 133 - 128 = 5 valid bits in word 4

static int errors = 0, tests = 0;

static void tick(Vpenumbra2_fetch_buffer* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void expect(const char* name, bool cond) {
    tests++;
    if (!cond) { printf("  FAIL [%s]\n", name); errors++; }
}

// Per-tag, per-word signature: distinct across words so a mis-slotted or
// partially corrupt entry cannot accidentally match.
static uint32_t gen_word(uint32_t tag, int i) {
    static const uint32_t SALT[NWORDS] =
        {0x00000000u, 0x9E3779B9u, 0x3C6EF372u, 0xDAA61D2Bu, 0x00001234u};
    uint32_t w = tag ^ SALT[i];
    return (i == NWORDS - 1) ? (w & W4_MASK) : w;
}
static void set_payload(Vpenumbra2_fetch_buffer* dut, uint32_t tag) {
    for (int i = 0; i < NWORDS; i++) dut->i_enq_data[i] = gen_word(tag, i);
}
static bool head_is(Vpenumbra2_fetch_buffer* dut, uint32_t tag) {
    for (int i = 0; i < NWORDS; i++)
        if (dut->o_deq_data[i] != gen_word(tag, i)) return false;
    return true;
}

static void reset(Vpenumbra2_fetch_buffer* dut) {
    dut->i_rst = 1; dut->i_flush = 0;
    dut->i_enq_valid = 0; dut->i_deq_ready = 0; set_payload(dut, 0);
    tick(dut);
    dut->i_rst = 0;
}

// Drive one cycle's inputs and settle combinational outputs.
static void drive(Vpenumbra2_fetch_buffer* dut, bool ev, uint32_t tag,
                  bool dr, bool fl) {
    dut->i_enq_valid = ev; set_payload(dut, tag);
    dut->i_deq_ready = dr; dut->i_flush = fl;
    dut->eval();
}

int main() {
    Vpenumbra2_fetch_buffer* dut = new Vpenumbra2_fetch_buffer;
    reset(dut);

    // ── 1-cycle latency: enqueue into empty, head valid NEXT cycle ──
    drive(dut, /*ev*/1, 0xA1, /*dr*/0, /*fl*/0);
    expect("empty_ready",    dut->o_enq_ready == 1);
    expect("empty_no_valid", dut->o_deq_valid == 0);   // not visible same cycle
    tick(dut);                                          // enqueue lands
    drive(dut, 0, 0, 0, 0);
    expect("head_valid_next_cycle", dut->o_deq_valid == 1);
    expect("head_data_A1",          head_is(dut, 0xA1));

    // ── Fill to full: o_enq_ready drops at DEPTH ──
    drive(dut, 1, 0xB2, 0, 0);
    expect("ready_at_1", dut->o_enq_ready == 1);
    tick(dut);
    drive(dut, 0, 0, 0, 0);
    expect("full_not_ready",   dut->o_enq_ready == 0);
    expect("full_still_valid", dut->o_deq_valid == 1);

    // ── Drain in FIFO order: A1 then B2 ──
    drive(dut, 0, 0, /*dr*/1, 0);
    expect("drain_head_A1", head_is(dut, 0xA1));
    tick(dut);
    drive(dut, 0, 0, 1, 0);
    expect("drain_head_B2", head_is(dut, 0xB2));
    tick(dut);
    drive(dut, 0, 0, 0, 0);
    expect("empty_after_drain", dut->o_deq_valid == 0);

    // ── Simultaneous enqueue+dequeue (the classic FIFO corruption corner) ──
    // Each scenario sets up its own occupancy explicitly (no reliance on a
    // leftover from a previous block) and leaves the FIFO empty at the end.

    // (a) occupancy 1, push+pop the same cycle, several times. The head read
    //     this cycle is the OLD front while the new tag is written behind it;
    //     the new tag becomes the head next cycle. count holds at 1 throughout.
    char nm[48];
    drive(dut, 1, 0x10, 0, 0); tick(dut);                 // seed: [0x10]
    for (unsigned t = 0x11; t <= 0x14; t++) {
        drive(dut, 1, t, 1, 0);                           // push t + pop, same cycle
        expect("pp1_ready", dut->o_enq_ready == 1);
        expect("pp1_valid", dut->o_deq_valid == 1);
        snprintf(nm, sizeof nm, "pp1_old_head_%02X", t - 1);
        expect(nm, head_is(dut, t - 1));                  // old front out this cycle
        tick(dut);
        snprintf(nm, sizeof nm, "pp1_new_head_%02X", t);
        expect(nm, head_is(dut, t));                      // pushed tag is head next cycle
    }
    drive(dut, 0, 0, 1, 0); tick(dut);                    // drain the last entry
    drive(dut, 0, 0, 0, 0);
    expect("pp1_left_empty", dut->o_deq_valid == 0);

    // (b) full, push+pop the same cycle: o_enq_ready is 0, so the push is
    //     REJECTED — only the pop happens, and the rejected tag must be gone
    //     for good (re-offer it and confirm it then enqueues fresh).
    drive(dut, 1, 0x20, 0, 0); tick(dut);                 // [0x20]
    drive(dut, 1, 0x21, 0, 0); tick(dut);                 // [0x20,0x21] full
    drive(dut, 1, 0x22, 1, 0);                            // push 0x22 + pop, at full
    expect("pp2_push_rejected", dut->o_enq_ready == 0);   // 0x22 not accepted
    expect("pp2_old_head_20",   head_is(dut, 0x20));      // 0x20 pops out
    tick(dut);
    expect("pp2_new_head_21",   head_is(dut, 0x21));      // 0x21, NOT 0x22
    drive(dut, 1, 0x22, 1, 0);                            // room now: pop 0x21, push 0x22
    expect("pp2_head_21_again", head_is(dut, 0x21));
    tick(dut);
    expect("pp2_head_22_fresh", head_is(dut, 0x22));      // 0x22 finally lands
    drive(dut, 0, 0, 1, 0); tick(dut);                    // drain 0x22
    drive(dut, 0, 0, 0, 0);
    expect("pp2_left_empty", dut->o_deq_valid == 0);

    // ── Flush: empties in one cycle, overrides coincident enq/deq ──
    drive(dut, 1, 0xC3, 0, 0); tick(dut);                 // -> occupancy 1
    drive(dut, 0, 0, 0, 0);
    expect("preflush_valid", dut->o_deq_valid == 1);
    drive(dut, 0, 0, 0, /*fl*/1); tick(dut);              // flush
    drive(dut, 0, 0, 0, 0);
    expect("flush_empties",  dut->o_deq_valid == 0);
    expect("flush_ready",    dut->o_enq_ready == 1);

    drive(dut, /*ev*/1, 0xD4, 0, /*fl*/1); tick(dut);     // flush wins over enq
    drive(dut, 0, 0, 0, 0);
    expect("flush_over_enq", dut->o_deq_valid == 0);

    drive(dut, 1, 0xE5, 0, 0); tick(dut);                 // fill to full
    drive(dut, 1, 0xF6, 0, 0); tick(dut);
    drive(dut, 0, 0, 0, 0);
    expect("refill_full", dut->o_enq_ready == 0);
    drive(dut, 0, 0, /*dr*/1, /*fl*/1); tick(dut);        // flush wins over deq
    drive(dut, 0, 0, 0, 0);
    expect("flush_over_full_deq", dut->o_deq_valid == 0);

    // ── Randomized reference-model soak ──
    // Golden std::deque mirrors the FIFO; every cycle we cross-check the DUT's
    // ready/valid against occupancy and the visible head against the model
    // front. Flush clears the model and overrides this cycle's enq/deq, exactly
    // as the RTL does. This is the net that catches any push+pop corruption.
    std::deque<uint32_t> golden;
    uint32_t rng = 0xC0FFEEu;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    uint32_t next_tag = 0x1000;
    const int CYCLES = 20000;
    for (int c = 0; c < CYCLES && errors == 0; c++) {
        bool ev = rnd() & 1;
        bool dr = rnd() & 1;
        bool fl = (rnd() & 0x3F) == 0;       // ~1.5% of cycles
        uint32_t tag = next_tag;
        drive(dut, ev, tag, dr, fl);

        // Handshake outputs must track the model's occupancy exactly.
        expect("soak_enq_ready", (bool)dut->o_enq_ready == ((int)golden.size() < DEPTH));
        expect("soak_deq_valid", (bool)dut->o_deq_valid == (!golden.empty()));
        // The visible head must always equal the model's front when non-empty.
        if (!golden.empty()) expect("soak_head_data", head_is(dut, golden.front()));

        bool enq_fire = ev && dut->o_enq_ready;
        bool deq_fire = dut->o_deq_valid && dr;
        tick(dut);

        if (fl) {
            golden.clear();                  // flush wins, no enq/deq this edge
        } else {
            if (deq_fire) golden.pop_front();
            if (enq_fire) { golden.push_back(tag); next_tag++; }
        }
    }

    printf("penumbra2_fetch_buffer: %d/%d checks passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return errors ? 1 : 0;
}

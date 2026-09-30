// Verilator testbench for the gen3 L1 lookup (penumbra3_l1_lookup_test:
// penumbra3_l1_store feeding penumbra3_l1_match).
//
// Timing: a lookup launches in cycle A (i_launch with the set and word);
// after one tick the registered set is in the resolve cycle B, where the
// testbench drives the frame candidates and reads the verdict.
//
// Covers: an empty cache misses; a line-wide fill installs tag, every word
// and valid in one write; the frame select picks which candidate the tags
// compare against, so a matching frame that is not selected does not hit;
// each way and each word answer; a store writes only its enabled bytes; a
// launch reading the word a store writes in the same cycle finds that way
// invalid (the other way unaffected) and the store still lands; a launch
// reading another word, or the same word in another set, is not blocked;
// invalidate-all.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_l1_lookup_test.h"
#include "verilated.h"

typedef Vpenumbra3_l1_lookup_test Dut;

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Dut* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}
static void idle(Dut* d) {
    d->i_launch = 0; d->i_fill_we = 0; d->i_store_we = 0; d->i_inval_all = 0;
    d->i_frame = 0; d->i_frame_sel = 0;
}
// Candidate k's frame and the one-hot select, as the MMU drives them.
static void frames(Dut* d, uint32_t f0, uint32_t f1, uint32_t f2, uint32_t sel) {
    d->i_frame = (uint64_t)f0 | ((uint64_t)f1 << 20) | ((uint64_t)f2 << 40);
    d->i_frame_sel = sel;
    d->eval();
}
static uint32_t line_word(uint32_t base, int c) { return base + c; }
static void fill(Dut* d, uint32_t set, uint32_t way, uint32_t tag, uint32_t base) {
    idle(d);
    d->i_fill_we = 1; d->i_fill_set = set; d->i_fill_way = way; d->i_fill_tag = tag;
    for (int c = 0; c < 4; c++) d->i_fill_line[c] = line_word(base, c);
    tick(d);
    idle(d);
}
// Launch a lookup, with any store write set up for the same cycle.
static void launch(Dut* d, uint32_t set, uint32_t word) {
    d->i_launch = 1; d->i_set = set; d->i_word = word;
    tick(d);
    d->i_launch = 0; d->i_store_we = 0;
    d->eval();
}
// Set up a store write for the next tick.
static void store(Dut* d, uint32_t set, uint32_t word, uint32_t way,
                  uint32_t data, uint32_t be) {
    d->i_store_we = 1; d->i_store_set = set; d->i_store_word = word;
    d->i_store_way = way; d->i_store_wdata = data; d->i_store_byte_en = be;
}
// A store write in a cycle with no launch.
static void store_alone(Dut* d, uint32_t set, uint32_t word, uint32_t way,
                        uint32_t data, uint32_t be) {
    store(d, set, word, way, data, be);
    tick(d);
    d->i_store_we = 0; d->eval();
}

enum { TAG_A = 0x12345, TAG_B = 0xABCDE, TAG_X = 0x55555 };
enum { BASE_A = 0xA0000000, BASE_B = 0xB0000000 };

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;

    idle(d); d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();

    // ── Empty: nothing hits ──
    launch(d, 5, 2); frames(d, TAG_A, 0, 0, 0b001);
    check("empty_miss", d->o_hit, 0);

    // ── Two lines in one set, one per way ──
    fill(d, 5, 0, TAG_A, BASE_A);
    fill(d, 5, 1, TAG_B, BASE_B);

    launch(d, 5, 2);
    frames(d, TAG_A, 0, 0, 0b001);
    check("early_hit",      d->o_hit, 1);
    check("early_way",      d->o_way_hit, 0b01);
    check("early_word",     d->o_word, line_word(BASE_A, 2));
    frames(d, TAG_B, 0, TAG_A, 0b100);
    check("tlbway_hit",     d->o_hit, 1);
    check("tlbway_way",     d->o_way_hit, 0b01);
    frames(d, TAG_B, TAG_X, TAG_A, 0b010);
    check("unselected_miss", d->o_hit, 0);
    frames(d, TAG_A, 0, TAG_B, 0b100);
    check("way1_hit",       d->o_way_hit, 0b10);
    check("way1_word",      d->o_word, line_word(BASE_B, 2));
    frames(d, TAG_A, TAG_A, TAG_A, 0b000);
    check("tlb_miss",       d->o_hit, 0);

    // ── Each word column ──
    for (uint32_t w = 0; w < 4; w++) {
        launch(d, 5, w); frames(d, TAG_A, 0, 0, 0b001);
        check("column_word", d->o_word, line_word(BASE_A, w));
    }

    // ── A store writes only its enabled bytes ──
    store_alone(d, 5, 1, 0, 0xDEADBEEF, 0b0011);
    launch(d, 5, 1); frames(d, TAG_A, 0, 0, 0b001);
    check("store_bytes", d->o_word, (line_word(BASE_A, 1) & 0xFFFF0000) | 0xBEEF);

    // ── A store writing the launched word blocks that way, and lands ──
    store(d, 5, 2, 0, 0x11112222, 0b1111);
    launch(d, 5, 2);
    frames(d, TAG_A, 0, 0, 0b001);
    check("blocked_way_misses", d->o_hit, 0);
    frames(d, TAG_B, 0, 0, 0b001);
    check("other_way_hits",     d->o_way_hit, 0b10);
    check("other_way_word",     d->o_word, line_word(BASE_B, 2));
    launch(d, 5, 2); frames(d, TAG_A, 0, 0, 0b001);
    check("blocked_store_landed", d->o_word, 0x11112222);

    // ── Another word, or the same word in another set, is not blocked ──
    store(d, 5, 3, 0, 0x33334444, 0b1111);
    launch(d, 5, 2);
    frames(d, TAG_A, 0, 0, 0b001);
    check("other_word_hit",  d->o_hit, 1);
    check("other_word_word", d->o_word, 0x11112222);
    launch(d, 5, 3); frames(d, TAG_A, 0, 0, 0b001);
    check("other_word_landed", d->o_word, 0x33334444);
    fill(d, 6, 0, TAG_A, BASE_A + 0x100);
    store(d, 6, 3, 0, 0x55556666, 0b1111);
    launch(d, 5, 3);
    frames(d, TAG_A, 0, 0, 0b001);
    check("other_set_hit",   d->o_hit, 1);
    check("other_set_word",  d->o_word, 0x33334444);
    launch(d, 6, 3); frames(d, TAG_A, 0, 0, 0b001);
    check("other_set_landed", d->o_word, 0x55556666);

    // ── Invalidate-all ──
    d->i_inval_all = 1; tick(d); idle(d);
    launch(d, 5, 2); frames(d, TAG_A, 0, TAG_B, 0b001);
    check("inval_miss_a", d->o_hit, 0);
    frames(d, TAG_A, 0, TAG_B, 0b100);
    check("inval_miss_b", d->o_hit, 0);

    printf("penumbra3_l1_lookup: %d/%d checks passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}

// Verilator testbench for penumbra3_l1 (a gen3 L1 cache: storage, the hit
// verdict and the cache device).
//
// The cache has no request side, so the testbench plays its consumer and
// the MMU: it launches lookups, drives the frame candidates in the resolve
// cycle, raises a store or a count strobe there, installs lines, and drives
// the sysreg port.
//
// Timing (the module's contract): a launch is sampled at a rising edge, and
// the cycle after is that lookup's resolve, where the verdict is
// combinational in the frame candidates. The lookup stays the current one
// until the next launch, and the cache reads its set again at every edge.
// A store hit is written at the edge that ends the cycle after its
// resolve, and a replacement touch an edge later.
//
// Covers: reset (disabled, nothing valid); INFO and CTRL; a line installed
// in one write answers every word; the frame select decides which
// candidate the tags compare against; both ways of a set resident; a store
// hit merges its bytes, a store miss allocates nothing; a lookup of a word
// a store is writing is given the stored word in the cycle of the write,
// in the cycle after, and from then on, whether it was current before the
// write or launches during it; the younger of two stores wins; stores to
// two ways at one set and word each reach their own way; a store reaches
// no lookup of another word, way or set; a lookup that stays current keeps
// its verdict, and follows a line installed or an INVAL_ALL under it;
// invalid-first then tree-PLRU victims; a disabled cache neither hits nor
// absorbs a store nor counts; the four counters; INVAL_ALL.
//
// The module's assertions run under --assert throughout: one-hot hits and
// frame select, the fill exclusions, and the no_rw_check invariant on
// every data column.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_l1.h"
#include "verilated.h"

// Geometry: the module's defaults (8 KB 2-way, 16-byte lines).
static const int LINE_WORDS = 4, NUM_WAYS = 2, NUM_SETS = 256;

// Unified cache sysreg map (penumbra_pkg.sv).
static const int REG_INFO = 0, REG_CTRL = 1, REG_INVAL_ALL = 2;
static const int REG_READ_HITS = 10, REG_READ_MISSES = 11;
static const int REG_WRITE_HITS = 12, REG_WRITE_MISSES = 13;
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

// Drop every strobe. The frame candidates stay: the current lookup keeps
// resolving against them.
static void quiet() {
    d->i_launch = 0; d->i_store = 0;
    d->i_count_read = 0; d->i_count_write = 0;
    d->i_fill_we = 0; d->i_sys_we = 0; d->i_sys_re = 0;
    d->eval();
}

// Candidates as the MMU drives them: the early frame (bypass or pinned) is
// candidate 0, the main TLB's ways 1 and 2.
static void frames(uint32_t f0, uint32_t f1, uint32_t f2, uint32_t sel) {
    d->i_frame = (uint64_t)f0 | ((uint64_t)f1 << 20) | ((uint64_t)f2 << 40);
    d->i_frame_sel = sel;
    d->eval();
}
static void frame(uint32_t pa) { frames(pa >> 12, 0, 0, 0b001); }

// Raise a launch at pa's page offset for the next edge.
static void launch_at(uint32_t pa) {
    d->i_launch = 1; d->i_offset = pa & 0xFFF; d->eval();
}

// Launch, tick into the resolve, present pa's frame. Leaves the DUT in the
// resolve cycle.
static bool lookup(uint32_t pa, uint32_t* word = nullptr) {
    quiet(); launch_at(pa); tick(); quiet(); frame(pa);
    if (word) *word = d->o_word;
    return d->o_hit != 0;
}

// Two edges with nothing launched: a pending store write and its touch land.
static void settle() { quiet(); tick(); tick(); d->eval(); }

static uint32_t line_word(uint32_t base, int c) { return base + c; }

// A fill installing in this cycle.
static void fill_now(uint32_t pa, int way, uint32_t base) {
    d->i_fill_we = 1; d->i_fill_paddr = pa; d->i_fill_way = way;
    for (int c = 0; c < LINE_WORDS; c++) d->i_fill_line[c] = line_word(base, c);
    d->eval();
}

static void fill(uint32_t pa, int way, uint32_t base) {
    quiet(); fill_now(pa, way, base); tick(); quiet();
}

// A store resolving in this cycle.
static void store_now(uint32_t data, uint32_t be) {
    d->i_store = 1; d->i_store_wdata = data; d->i_store_byte_en = be; d->eval();
}

// A store on its own: returns whether it hit, with its write landed.
static bool store(uint32_t pa, uint32_t data, uint32_t be) {
    bool hit = lookup(pa);
    store_now(data, be); tick(); settle();
    return hit;
}

// INVAL_ALL is registered, so the trailing tick lets the clear land.
static void sys_write(int reg, uint32_t v) {
    quiet(); d->i_sys_we = 1; d->i_sys_reg = reg; d->i_sys_wdata = v;
    tick(); quiet(); tick();
}

// The RDSYS launch registers the selector; the response follows it.
static uint32_t sys_read(int reg) {
    quiet(); d->i_sys_re = 1; d->i_sys_reg = reg; tick(); quiet();
    return d->o_sys_rdata;
}

static void set_enable(bool on) { sys_write(REG_CTRL, on ? 1 : 0); }

// ── Test addresses ──────────────────────────────────────────────
// All three lines index set 5 and differ only in the frame, so they share
// a set while the virtual and physical offsets agree.
static const uint32_t PA_A = 0x00010050, PA_B = 0x00020050, PA_C = 0x00030050;
static const uint32_t BASE_A = 0xA0000000, BASE_B = 0xB0000000, BASE_C = 0xC0000000;

// Line A's frame, the next set.
static const uint32_t PA_D = 0x00010060, BASE_D = 0xD0000000;

static void test_lookup() {
    // ── Miss, install, every word hits ──
    check("a_miss", lookup(PA_A), 0);
    check("a_victim_invalid_first", d->o_victim_way, 0);
    settle();
    fill(PA_A, 0, BASE_A);
    for (int w = 0; w < LINE_WORDS; w++) {
        uint32_t word = 0;
        char n[32];
        snprintf(n, sizeof n, "a_hit_w%d", w);
        check(n, lookup(PA_A + 4 * w, &word), 1);
        snprintf(n, sizeof n, "a_word_w%d", w);
        check(n, word, line_word(BASE_A, w));
        snprintf(n, sizeof n, "a_way_w%d", w);
        check(n, d->o_way_hit, 0b01);
        snprintf(n, sizeof n, "a_way_word_w%d", w);
        check(n, (uint32_t)d->o_way_word, line_word(BASE_A, w));
        settle();
    }

    // ── The frame select decides which candidate compares ──
    quiet(); launch_at(PA_A + 8); tick(); quiet();
    frames(0x99, 0x10, 0x77, 0b010);
    check("tlbway_hit", d->o_hit, 1);
    check("tlbway_word", d->o_word, line_word(BASE_A, 2));
    frames(0x10, 0x99, 0x77, 0b010);
    check("unselected_miss", d->o_hit, 0);
    frames(0x10, 0x10, 0x10, 0b000);
    check("tlb_miss", d->o_hit, 0);
    settle();

    // ── Same set, another frame: the second way ──
    check("b_miss", lookup(PA_B), 0);
    check("b_victim_invalid_first", d->o_victim_way, 1);
    settle();
    fill(PA_B, 1, BASE_B);
    check("a_still_hit", lookup(PA_A), 1);
    settle();
    uint32_t word = 0;
    check("b_hit", lookup(PA_B, &word), 1);
    check("b_way", d->o_way_hit, 0b10);
    check("b_word", word, line_word(BASE_B, 0));
    check("b_way_words", (uint32_t)(d->o_way_word >> 32), line_word(BASE_B, 0));
    settle();
}

static void test_store() {
    // ── A hit merges its bytes; a miss allocates nothing ──
    check("st_hit", store(PA_A + 4, 0xDEADBEEF, 0x3), 1);
    uint32_t word = 0;
    check("st_read_back", lookup(PA_A + 4, &word), 1);
    check("st_merged", word, (line_word(BASE_A, 1) & 0xFFFF0000) | 0xBEEF);
    settle();

    check("stm_miss", store(PA_C, 0x5A5A5A5A, 0xF), 0);
    check("stm_not_allocated", lookup(PA_C), 0);
    settle();
}

// A store hit is written in the cycle after the store's own lookup, and
// the block RAM shows the word two cycles after that write. A lookup of the
// word that is current in the write's cycle is given it from the store
// port (the module's store_now); one that is current in the next cycle,
// from the register behind the port (store_next, fwd_word_q); after that
// the RAM has it. Check names say which: now, next, ram.
static void test_store_forward() {
    uint32_t word = 0;

    // ── A load directly behind the store: it launches as the store
    // resolves, so the write falls in its first cycle on the outputs, and
    // it stays current for two cycles more ──
    quiet(); launch_at(PA_A + 8); tick();                    // the store launches
    quiet(); frame(PA_A); store_now(0x11112222, 0xF);
    launch_at(PA_A + 8); tick();                             // the load launches
    quiet(); frame(PA_A);                                    // the store is written this cycle
    check("now_hit", d->o_hit, 1);
    check("now_word", d->o_word, 0x11112222);
    tick();                                                  // the store port has moved on
    check("now_then_next", d->o_word, 0x11112222);
    tick();
    check("now_then_ram", d->o_word, 0x11112222);
    settle();

    // A partial store merges into the word the load is given.
    quiet(); launch_at(PA_A + 12); tick();
    quiet(); frame(PA_A); store_now(0x000000AB, 0x1);
    launch_at(PA_A + 12); tick();
    quiet(); frame(PA_A);
    check("now_partial", d->o_word, (line_word(BASE_A, 3) & 0xFFFFFF00) | 0xAB);
    settle();

    // ── A load that launches in the store's write cycle: its read shares
    // the write's edge ──
    quiet(); launch_at(PA_B + 4); tick();                    // the store launches
    quiet(); frame(PA_B); store_now(0x33334444, 0xF);
    launch_at(PA_A); tick();                                 // an unrelated access between
    quiet(); frame(PA_A);                                    // the store is written this cycle
    check("between_hit", d->o_hit, 1);
    check("between_word", d->o_word, line_word(BASE_A, 0));
    launch_at(PA_B + 4); tick();                             // the load reads at the write's edge
    quiet(); frame(PA_B);
    check("next_hit", d->o_hit, 1);
    check("next_word", d->o_word, 0x33334444);
    frame(PA_A);                                             // the other way, same word: untouched
    check("next_other_way", d->o_word, (line_word(BASE_A, 1) & 0xFFFF0000) | 0xBEEF);
    frame(PA_B);
    tick();
    check("next_then_ram", d->o_word, 0x33334444);
    settle();
    check("landed_hit", lookup(PA_B + 4, &word), 1);
    check("landed_word", word, 0x33334444);
    settle();

    // ── The cycle after a write, the stored word goes to the lookup that
    // is current then, not to the one that was current during the write:
    // a load of the stored word, then a load of another word of the line
    // launching in the write's cycle ──
    quiet(); launch_at(PA_A + 8); tick();                    // the store launches
    quiet(); frame(PA_A); store_now(0x55551111, 0xF);
    launch_at(PA_A + 8); tick();                             // a load of the stored word
    quiet(); frame(PA_A);                                    // the store is written this cycle
    check("now_before_launch", d->o_word, 0x55551111);
    launch_at(PA_A); tick();                                 // another word launches at the write's edge
    quiet(); frame(PA_A);
    check("next_other_word", d->o_word, line_word(BASE_A, 0));
    settle();

    // ── Two stores to one word, then a load: the younger store wins ──
    // The load reads at store 1's write, and store 2 is written in the
    // load's first cycle on the outputs. Store 2's own lookup was given
    // store 1's word, and merged its bytes over that.
    quiet(); launch_at(PA_A); tick();                        // store 1 launches
    quiet(); frame(PA_A); store_now(0x0000CAFE, 0x3);
    launch_at(PA_A); tick();                                 // store 2 launches
    quiet(); frame(PA_A); store_now(0xF00D0000, 0xC);
    launch_at(PA_A); tick();                                 // the load reads at store 1's write
    quiet(); frame(PA_A);                                    // store 2 is written this cycle
    check("younger_word", d->o_word, 0xF00DCAFE);
    tick();
    check("younger_then_next", d->o_word, 0xF00DCAFE);
    settle();
    check("younger_landed_hit", lookup(PA_A, &word), 1);
    check("younger_landed_word", word, 0xF00DCAFE);
    settle();

    // ── Stores to both ways at one set and word: each to its own way ──
    // The load reads at store A's write (way 0), and store B (way 1) is
    // written in the load's first cycle on the outputs. A cycle later
    // way 0 is the RAM's and way 1 comes from the register, which by then
    // holds store B's word.
    quiet(); launch_at(PA_A + 8); tick();                    // store A launches
    quiet(); frame(PA_A); store_now(0xAAAA0001, 0xF);
    launch_at(PA_B + 8); tick();                             // store B launches
    quiet(); frame(PA_B); store_now(0xBBBB0002, 0xF);
    launch_at(PA_A + 8); tick();                             // the load reads at store A's write
    quiet();                                                 // store B is written this cycle
    frame(PA_A);
    check("ways_next_hit", d->o_way_hit, 0b01);
    check("ways_next_word", d->o_word, 0xAAAA0001);
    frame(PA_B);
    check("ways_now_hit", d->o_way_hit, 0b10);
    check("ways_now_word", d->o_word, 0xBBBB0002);
    tick();
    frame(PA_A);
    check("ways_next_then_ram", d->o_word, 0xAAAA0001);
    frame(PA_B);
    check("ways_now_then_next", d->o_word, 0xBBBB0002);
    settle();

    // ── A store to another word or another set reaches no lookup ──
    quiet(); launch_at(PA_A + 4); tick();
    quiet(); frame(PA_A); store_now(0x55556666, 0xF);
    launch_at(PA_A + 8); tick();                             // another word of the line
    quiet(); frame(PA_A);
    check("other_word", d->o_word, 0xAAAA0001);
    settle();
    fill(PA_D, 0, BASE_D);
    quiet(); launch_at(PA_A + 4); tick();
    quiet(); frame(PA_A); store_now(0x77778888, 0xF);
    launch_at(PA_D + 4); tick();                             // the same word and way, set 6
    quiet(); frame(PA_D);
    check("other_set_hit", d->o_hit, 1);
    check("other_set_word", d->o_word, line_word(BASE_D, 1));
    settle();
}

// A lookup stays current while nothing launches, whatever the launch
// address does, and the cache reads its set again every cycle: the verdict
// stays while the cache does, and follows it when it changes.
static void test_hold() {
    uint32_t word = 0;
    check("hold_setup", lookup(PA_A + 8, &word), 1);
    check("hold_setup_word", word, 0xAAAA0001);
    d->i_offset = (PA_B + 12) & 0xFFF;
    tick(); tick(); d->eval();
    check("hold_hit", d->o_hit, 1);
    check("hold_word", d->o_word, 0xAAAA0001);
    check("after_hold", lookup(PA_B + 12, &word), 1);
    check("after_hold_word", word, line_word(BASE_B, 3));
    settle();

    // A line installed into the current lookup's set shows two cycles
    // after its fill: the fill's own edge, then the read that finds it.
    // The cache says so in the fill's cycle.
    check("held_fill_miss", lookup(PA_C + 4), 0);
    uint32_t victim = d->o_victim_way;
    fill_now(PA_C, victim, BASE_C);
    check("held_fill_stale_next", d->o_stale_next, 1);
    tick(); quiet();
    check("held_fill_between_miss", d->o_hit, 0);
    check("held_fill_between_quiet", d->o_stale_next, 0);
    tick(); d->eval();
    check("held_fill_hit", d->o_hit, 1);
    check("held_fill_word", d->o_word, line_word(BASE_C, 1));
    settle();

    // An install into another set leaves the current lookup as it is.
    check("held_other_fill_setup", lookup(PA_C + 4), 1);
    fill_now(PA_D, 0, BASE_D);                               // set 6
    check("held_other_fill_stale_next", d->o_stale_next, 0);
    tick(); quiet();
    check("held_other_fill_hit", d->o_hit, 1);
    check("held_other_fill_word", d->o_word, line_word(BASE_C, 1));
    settle();

    // INVAL_ALL reaches a lookup that stays current.
    check("held_inval_setup", lookup(PA_C + 4), 1);
    sys_write(REG_INVAL_ALL, 0);
    tick(); d->eval();
    check("held_inval_miss", d->o_hit, 0);
    settle();
}

static void test_replacement() {
    sys_write(REG_INVAL_ALL, 0);
    fill(PA_A, 0, BASE_A);
    fill(PA_B, 1, BASE_B);
    settle();
    // Installed 0 then 1, so way 0 is the less recently used.
    check("lru_after_fills", lookup(PA_C), 0);
    check("lru_after_fills_way", d->o_victim_way, 0);
    settle();
    check("lru_touch_a", lookup(PA_A), 1);
    settle();
    check("lru_after_a", lookup(PA_C), 0);
    check("lru_after_a_way", d->o_victim_way, 1);
    settle();
    check("lru_touch_b", lookup(PA_B), 1);
    settle();
    check("lru_after_b", lookup(PA_C), 0);
    check("lru_after_b_way", d->o_victim_way, 0);
    settle();
}

static uint32_t counter(int reg) { return sys_read(reg); }

static void test_counters_and_disable() {
    settle();
    uint32_t rh = counter(REG_READ_HITS), rm = counter(REG_READ_MISSES);
    uint32_t wh = counter(REG_WRITE_HITS), wm = counter(REG_WRITE_MISSES);

    lookup(PA_A); d->i_count_read = 1; d->eval(); tick(); settle();
    lookup(PA_C); d->i_count_read = 1; d->eval(); tick(); settle();
    lookup(PA_A); store_now(0x12345678, 0xF); d->i_count_write = 1; d->eval(); tick(); settle();
    lookup(PA_C); store_now(0x12345678, 0xF); d->i_count_write = 1; d->eval(); tick(); settle();

    check("cnt_read_hits",    counter(REG_READ_HITS)    - rh, 1);
    check("cnt_read_misses",  counter(REG_READ_MISSES)  - rm, 1);
    check("cnt_write_hits",   counter(REG_WRITE_HITS)   - wh, 1);
    check("cnt_write_misses", counter(REG_WRITE_MISSES) - wm, 1);

    // ── Disabled: no hit, no store absorbed, nothing counted ──
    set_enable(false);
    check("dis_enabled_out", d->o_enabled, 0);
    check("dis_ctrl", sys_read(REG_CTRL), 0);
    check("dis_no_hit", lookup(PA_A), 0);
    d->i_count_read = 1; d->eval(); tick(); settle();
    check("dis_store_miss", store(PA_A, 0x99999999, 0xF), 0);
    check("dis_not_counted", counter(REG_READ_MISSES) - rm, 1);

    // The line survives the disable, unchanged by the store made meanwhile.
    set_enable(true);
    uint32_t word = 0;
    check("reen_hit", lookup(PA_A, &word), 1);
    check("reen_word", word, 0x12345678);
    settle();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Vpenumbra3_l1;

    quiet();
    d->i_rst = 1; tick(); tick(); d->i_rst = 0; d->eval();

    // ── Reset: disabled, nothing valid ──
    check("rst_enabled", d->o_enabled, 0);
    check("rst_ctrl", sys_read(REG_CTRL), 0);
    check("rst_miss", lookup(PA_A), 0);
    settle();

    check("info", sys_read(REG_INFO),
          ((uint32_t)CACHE_ADDR_VIPT << 26) | ((uint32_t)NUM_WAYS << 21)
          | ((uint32_t)NUM_SETS << 6) | (uint32_t)LINE_WORDS);
    set_enable(true);
    check("ctrl", sys_read(REG_CTRL), 1);
    check("enabled_out", d->o_enabled, 1);

    test_lookup();
    test_store();
    test_store_forward();
    test_hold();
    test_replacement();
    test_counters_and_disable();

    // ── INVAL_ALL drops every line ──
    sys_write(REG_INVAL_ALL, 0);
    check("inval_a", lookup(PA_A), 0);
    settle();
    check("inval_b", lookup(PA_B), 0);
    check("inval_victim", d->o_victim_way, 0);
    settle();

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}

// tb_ddr3_stack_test — the DDR3 memory stack from the system bus, on a
// Wishbone burst memory with random stall and ack latency.
//
// Drives ddr3_stack_test (ddr3_stack + wb_line_mem) the way the machine's
// bus drives memory: one access at a time, the strobe held until ack and
// through the ack cycle. The bus and the memory run on unrelated clocks
// (half-periods 7 and 5), so the crossing sees every phase. Every read is
// checked against a shadow of what was written.
//
// Covers:
//  - whole words over many lines: line misses, then hits in the held line;
//  - every byte, halfword and word position of a line, alone and beside
//    its neighbours: the write word's lanes and byte selects;
//  - a random mix of reads and sub-word writes over a few hundred lines,
//    under the model's random stall and latency;
//  - resets of the stack, in both domains, that leave a request with the
//    memory, which is not reset and goes on to answer it: no answer to a
//    request from before a reset completes one made after it, and every
//    one made after it completes. The request left is a read or a write,
//    and the reset anything from one bus cycle to longer than the memory
//    takes, alone or followed closely by a second;
//  - that the run actually saw requests held under stall, stall stretches
//    long enough to be a refresh, and an abandoned request's answer at
//    each point it can arrive.
#include "Vddr3_stack_test.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

static const uint32_t SPAN = 4096 * 16;    // the bytes the test addresses

static Vddr3_stack_test* d;
static uint64_t t = 0;
static int checks = 0, fails = 0;
static std::vector<uint8_t> shadow(SPAN, 0);
static std::mt19937 rng(12345);

// ── The memory port, as the memory acts on it ─────────────────────
// Sampled just ahead of each rising controller edge, where the values
// that edge acts on have settled.

// A request the memory has taken and not answered yet.
struct Taken {
    bool we;
    int resets;         // stack resets since the memory took it
};
static std::deque<Taken> in_memory;
static uint64_t taken_total = 0;

static uint64_t stb_under_stall = 0;
static uint64_t stall_run = 0, longest_stall = 0;

// Answers to requests made since the last reset, counted from it.
static uint64_t live_acks = 0;

// Answers to requests a reset abandoned, by when they arrived.
static uint64_t abandoned_in_reset = 0;         // while the stack was in reset
static uint64_t abandoned_before_next = 0;      // after it, the memory holding nothing newer
static uint64_t abandoned_behind_live = 0;      // after it, behind a request made since
static uint64_t abandoned_write_behind_live = 0;
static uint64_t twice_abandoned_behind_live = 0;

static int reset_ctrl_cycles = 0;               // the last reset, in controller cycles
static int last_behind_live_resets = 0;         // resets the last such answer's request outlived
static int last_behind_live_cycles = 0;         // and the last of them, in controller cycles

static void watch_port() {
    if (d->i_ctrl_rst)
        reset_ctrl_cycles++;

    if (d->o_wb_stall) {
        stall_run++;
        if (stall_run > longest_stall)
            longest_stall = stall_run;
    } else {
        stall_run = 0;
    }
    if (d->o_wb_stb && d->o_wb_stall)
        stb_under_stall++;

    if (d->o_wb_ack) {
        if (in_memory.empty()) {
            printf("  FAIL: the memory answered a request it never took\n");
            exit(1);
        }
        Taken r = in_memory.front();
        in_memory.pop_front();
        if (r.resets == 0) {
            live_acks++;
        } else if (d->i_ctrl_rst) {
            abandoned_in_reset++;
        } else {
            bool live_waiting = false;
            for (const Taken& later : in_memory)
                live_waiting |= (later.resets == 0);
            if (!live_waiting) {
                abandoned_before_next++;
            } else {
                abandoned_behind_live++;
                abandoned_write_behind_live += r.we;
                twice_abandoned_behind_live += (r.resets >= 2);
                last_behind_live_resets = r.resets;
                last_behind_live_cycles = reset_ctrl_cycles;
            }
        }
    }

    // A request taken on the edge that resets the stack is abandoned as
    // it is taken.
    if (d->o_wb_stb && !d->o_wb_stall) {
        in_memory.push_back({(bool)d->o_wb_we, d->i_ctrl_rst ? 1 : 0});
        taken_total++;
    }
}

static void chk(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        fails++;
        if (fails <= 20)
            printf("  FAIL: %s\n", what.c_str());
    }
}

static void step() {
    t++;
    bool sys_edge = (t % 7 == 0), ctrl_edge = (t % 5 == 0);
    if (!sys_edge && !ctrl_edge)
        return;
    if (ctrl_edge && !d->i_ctrl_clk) {
        d->eval();
        watch_port();
    }
    if (sys_edge)
        d->i_sys_clk = !d->i_sys_clk;
    if (ctrl_edge)
        d->i_ctrl_clk = !d->i_ctrl_clk;
    d->eval();
}

// To the next rising edge of the system clock.
static void tick() {
    for (;;) {
        bool before = d->i_sys_clk;
        step();
        if (!before && d->i_sys_clk)
            return;
    }
}

// Everything, the memory included: only at the start.
static void reset_all(int cycles) {
    d->i_sys_rst = 1;
    d->i_ctrl_rst = 1;
    d->i_mem_rst = 1;
    for (int i = 0; i < cycles; i++)
        tick();
    d->i_sys_rst = 0;
    d->i_ctrl_rst = 0;
    d->i_mem_rst = 0;
    in_memory.clear();
    live_acks = 0;
}

// The stack, in both domains, for `ticks` bus cycles; the memory is left
// alone, as the machine's DDR3 controller is.
static void stack_reset(int ticks) {
    d->i_we = 0;
    d->i_re = 0;
    d->i_sys_rst = 1;
    d->i_ctrl_rst = 1;
    for (Taken& r : in_memory)
        r.resets++;
    live_acks = 0;
    reset_ctrl_cycles = 0;
    for (int i = 0; i < ticks; i++)
        tick();
    d->i_sys_rst = 0;
    d->i_ctrl_rst = 0;
    tick();
}

// An access that never completes leaves the stack in no state to test
// further, so the run ends on it.
static void stuck(const std::string& what) {
    printf("  FAIL: %s never completed. Since the last reset the memory has answered\n"
           "        %llu request(s) made after it%s\n",
           what.c_str(), (unsigned long long)live_acks,
           live_acks ? ", so the stack is dropping answers it should take."
                     : ": none reached it, or it gave none.");
    printf("FAIL: stopped at an access that never completed\n");
    exit(1);
}

static bool wait_ack() {
    int w = 0;
    do {
        tick();
        if (++w > 20000)
            return false;
    } while (!d->o_ack);
    return true;
}

static void finish_access() {
    tick();                 // the ack cycle, strobe held
    d->i_we = 0;
    d->i_re = 0;
    tick();
}

static void start_write(uint32_t addr, uint32_t value, int size) {
    int lane = addr & 3;
    uint32_t mask = (size == 4) ? 0xF : (size == 2) ? 0x3 : 0x1;
    d->i_addr = addr & ~3u;
    d->i_wdata = value << (8 * lane);
    d->i_byte_en = mask << lane;
    d->i_we = 1;
    d->i_re = 0;
}

static void start_read(uint32_t addr) {
    d->i_addr = addr & ~3u;
    d->i_byte_en = 0xF;
    d->i_re = 1;
    d->i_we = 0;
}

// A write of `size` bytes (1, 2 or 4, aligned) at addr: the bytes in
// their lanes, the byte enables selecting them.
static void do_write(uint32_t addr, uint32_t value, int size) {
    start_write(addr, value, size);
    if (!wait_ack())
        stuck("the write at " + std::to_string(addr));
    for (int b = 0; b < size; b++)
        shadow[addr + b] = (value >> (8 * b)) & 0xFF;
    finish_access();
}

static uint32_t shadow_word(uint32_t addr) {
    uint32_t a = addr & ~3u;
    return shadow[a] | (shadow[a + 1] << 8) | (shadow[a + 2] << 16) | ((uint32_t)shadow[a + 3] << 24);
}

static void check_read(uint32_t addr, const char* what) {
    start_read(addr);
    if (!wait_ack())
        stuck(std::string(what) + ": the read at " + std::to_string(addr & ~3u));
    uint32_t got = d->o_rdata;
    finish_access();
    char msg[160];
    snprintf(msg, sizeof msg, "%s: read 0x%05X got 0x%08X want 0x%08X", what, addr & ~3u,
             got, shadow_word(addr));
    chk(got == shadow_word(addr), msg);
}

// A read after a reset. A wrong word here has one cause: an answer to a
// request from before the reset, taken for this read's.
static void check_read_after_reset(uint32_t addr, const char* what) {
    uint64_t behind_before = abandoned_behind_live;
    start_read(addr);
    if (!wait_ack())
        stuck(std::string(what) + ": the read at " + std::to_string(addr & ~3u));
    uint32_t got = d->o_rdata;
    finish_access();
    char msg[400];
    if (abandoned_behind_live != behind_before)
        snprintf(msg, sizeof msg,
                 "%s: read 0x%05X got 0x%08X want 0x%08X.\n"
                 "        The memory answered an abandoned request behind this read's, and the\n"
                 "        stack took that answer for it. The abandoned request had outlived %d\n"
                 "        reset(s), the last held for %d controller cycle(s).",
                 what, addr & ~3u, got, shadow_word(addr), last_behind_live_resets,
                 last_behind_live_cycles);
    else
        snprintf(msg, sizeof msg, "%s: read 0x%05X got 0x%08X want 0x%08X", what, addr & ~3u,
                 got, shadow_word(addr));
    chk(got == shadow_word(addr), msg);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Vddr3_stack_test;
    d->i_byte_en = 0xF;
    reset_all(8);

    // ── Whole words over many lines ───────────────────────────────
    for (uint32_t a = 0x1000; a < 0x1400; a += 4)
        do_write(a, rng(), 4);
    for (uint32_t a = 0x1000; a < 0x1400; a += 4)
        check_read(a, "words");

    // ── Every lane of a line ──────────────────────────────────────
    // Each byte on its own, then each halfword, then each word, with a
    // read of the whole line after every write: a write's byte selects
    // must reach its bytes and no others.
    const uint32_t L = 0x2000;
    for (int size : {1, 2, 4}) {
        for (uint32_t off = 0; off < 16; off += size) {
            do_write(L + off, rng(), size);
            for (uint32_t w = 0; w < 16; w += 4)
                check_read(L + w, size == 1 ? "byte lanes" : size == 2 ? "halfword lanes" : "word lanes");
        }
    }

    // ── Random mix ────────────────────────────────────────────────
    for (int i = 0; i < 6000; i++) {
        uint32_t addr = 0x4000 + (rng() % (512 * 16));
        int size = 1 << (rng() % 3);
        addr &= ~(uint32_t)(size - 1);
        if (rng() % 2)
            do_write(addr, rng(), size);
        else
            check_read(addr, "random");
    }

    // ── Resets that leave a request with the memory ───────────────
    // An access is started and the stack reset under it, in both
    // domains; the memory is not, and answers what it had taken when its
    // latency says, during the reset or after it. The next read, of
    // another line, must get its own data, and so must one of the line
    // the reset caught.
    //
    // Most rounds reset once the memory has the request, so that its
    // answer is still to come; the rest reset a few bus cycles after the
    // strobe, wherever the request has got to. The reset lasts 1 to 12
    // bus cycles, short beside the memory's latency, or 60, longer than
    // it; some rounds follow it closely with a second.
    for (int r = 0; r < 1500; r++) {
        uint32_t left = 0x8000 + 16 * (rng() % 256);
        uint32_t next = 0x9000 + 16 * (rng() % 256);
        do_write(left, 0xAB000000u | r, 4);
        do_write(next, 0xCD000000u | r, 4);
        // Writes are posted: acked once the crossing has them, before
        // memory does, so a reset right behind one can lose it. Reading
        // back first waits them out (the stack completes in order).
        check_read(next, "before a reset");

        bool leave_write = (r % 4 == 3);
        uint64_t taken_before = taken_total;
        if (leave_write)
            start_write(left, 0xEE000000u | r, 4);
        else
            start_read(left);
        // The strobe lasts through the access's ack, as any access's
        // does; a write is acked long before the memory takes it.
        bool acked = false;
        auto held_tick = [&] {
            tick();
            if (acked) {
                d->i_we = 0;
                d->i_re = 0;
            }
            acked |= (bool)d->o_ack;
        };
        if (r % 6 == 5) {
            for (int i = 0; i < (int)(rng() % 6); i++)
                held_tick();
        } else {
            for (int w = 0; taken_total == taken_before && w < 2000; w++)
                held_tick();
            for (int i = 0; i < (int)(rng() % 3); i++)
                held_tick();
        }

        stack_reset(r % 13 == 12 ? 60 : 1 + r % 12);
        if (r % 5 == 4) {
            for (int i = 0; i < (int)(rng() % 2); i++)
                tick();
            stack_reset(1 + rng() % 2);
        }

        check_read_after_reset(next, "after a reset");
        // Whether the write the reset caught was made or not, this one
        // is made after it.
        if (leave_write)
            do_write(left, 0xAB100000u | r, 4);
        check_read_after_reset(left, "the line the reset caught, after it");
    }

    // ── What the run saw ──────────────────────────────────────────
    // Each case the checks above rely on has to have come up; one that
    // did not fails the run, and is named.
    chk(stb_under_stall > 0, "no request was ever held under stall");
    chk(longest_stall >= 30,
        "no refresh-length stall was seen: the longest lasted " + std::to_string(longest_stall) +
            " controller cycles, of the 30 asked for");

    chk(abandoned_in_reset > 0, "no abandoned request was answered during a reset");
    chk(abandoned_before_next > 0,
        "no abandoned request was answered after a reset, ahead of the next");
    chk(abandoned_behind_live > 0, "no abandoned request was answered behind a later one");
    chk(abandoned_write_behind_live > 0,
        "no abandoned write was answered behind a later request");
    chk(twice_abandoned_behind_live > 0,
        "no request abandoned by two resets was answered behind a later one");

    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    delete d;
    return fails ? 1 : 0;
}

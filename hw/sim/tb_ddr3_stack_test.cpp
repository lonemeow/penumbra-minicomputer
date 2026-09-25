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
//  - a reset while the memory still has a read in flight: the bus cycle
//    drops, nothing from before the reset completes a request after it,
//    and every later read is right. (A write acked just before a reset
//    is not covered: writes are posted, acked once the crossing takes
//    them, so the reset discards one still on its way, as it would on
//    any stack of this shape.)
//  - that the run actually saw requests held under stall and stall
//    stretches long enough to be a refresh.
#include "Vddr3_stack_test.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static const uint32_t SPAN = 4096 * 16;    // the model's storage, in bytes

static Vddr3_stack_test* d;
static uint64_t t = 0;
static int checks = 0, fails = 0;
static std::vector<uint8_t> shadow(SPAN, 0);
static std::mt19937 rng(12345);

// Coverage of the memory port's timing, sampled on controller edges.
static uint64_t stb_under_stall = 0;
static uint64_t stall_run = 0, longest_stall = 0;

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
    if (sys_edge)
        d->i_sys_clk = !d->i_sys_clk;
    if (ctrl_edge)
        d->i_ctrl_clk = !d->i_ctrl_clk;
    d->eval();
    if (ctrl_edge && d->i_ctrl_clk) {
        if (d->o_wb_stall && d->o_wb_cyc) {
            stall_run++;
            if (stall_run > longest_stall)
                longest_stall = stall_run;
        } else {
            stall_run = 0;
        }
        if (d->o_wb_stb && d->o_wb_stall)
            stb_under_stall++;
    }
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

// A write of `size` bytes (1, 2 or 4, aligned) at addr: the bytes in
// their lanes, the byte enables selecting them.
static void do_write(uint32_t addr, uint32_t value, int size) {
    int lane = addr & 3;
    uint32_t mask = (size == 4) ? 0xF : (size == 2) ? 0x3 : 0x1;
    d->i_addr = addr & ~3u;
    d->i_wdata = value << (8 * lane);
    d->i_byte_en = mask << lane;
    d->i_we = 1;
    d->i_re = 0;
    chk(wait_ack(), "write stuck at " + std::to_string(addr));
    for (int b = 0; b < size; b++)
        shadow[addr + b] = (value >> (8 * b)) & 0xFF;
    finish_access();
}

static uint32_t shadow_word(uint32_t addr) {
    uint32_t a = addr & ~3u;
    return shadow[a] | (shadow[a + 1] << 8) | (shadow[a + 2] << 16) | ((uint32_t)shadow[a + 3] << 24);
}

static void check_read(uint32_t addr, const char* what) {
    d->i_addr = addr & ~3u;
    d->i_byte_en = 0xF;
    d->i_re = 1;
    d->i_we = 0;
    bool acked = wait_ack();
    uint32_t got = d->o_rdata;
    finish_access();
    char msg[160];
    snprintf(msg, sizeof msg, "%s: read 0x%05X got 0x%08X want 0x%08X", what, addr & ~3u,
             got, shadow_word(addr));
    chk(acked && got == shadow_word(addr), msg);
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

    // ── Reset with a read in flight ───────────────────────────────
    // A read is started, and once the memory has taken it (the strobe
    // has been up at least long enough to pass the crossing), the stack
    // is reset in both domains before its ack is due; the memory is not,
    // as the machine's DDR3 controller is not. The bus cycle must drop,
    // and the next read, of another line, must get its own data.
    int cyc_dropped = 0;
    for (int r = 0; r < 40; r++) {
        uint32_t aborted = 0x8000 + 16 * (rng() % 256);
        uint32_t next = 0x9000 + 16 * (rng() % 256);
        do_write(aborted, 0xAB000000u | r, 4);
        do_write(next, 0xCD000000u | r, 4);
        // Writes are posted: acked once the crossing has them, before
        // memory does, so a reset right behind one can lose it. Reading
        // back first waits them out (the stack completes in order).
        check_read(next, "before a reset");
        d->i_addr = aborted;
        d->i_byte_en = 0xF;
        d->i_re = 1;
        d->i_we = 0;
        int w = 0;
        while (!d->o_wb_stb && w++ < 2000)
            tick();
        for (int i = 0; i < (int)(rng() % 3); i++)
            tick();
        d->i_re = 0;
        d->i_sys_rst = 1;
        d->i_ctrl_rst = 1;
        for (int i = 0; i < 3; i++) {
            tick();
            cyc_dropped += !d->o_wb_cyc;
        }
        d->i_sys_rst = 0;
        d->i_ctrl_rst = 0;
        tick();
        check_read(next, "after a reset");
        check_read(aborted, "the aborted line, after a reset");
    }
    chk(cyc_dropped > 0, "the bus cycle never dropped during reset");

    // ── The timing the run saw ────────────────────────────────────
    char cov[160];
    snprintf(cov, sizeof cov, "requests held under stall: %llu controller cycles, longest stall %llu",
             (unsigned long long)stb_under_stall, (unsigned long long)longest_stall);
    printf("  %s\n", cov);
    chk(stb_under_stall > 0, "no request was ever held under stall");
    chk(longest_stall >= 30, "no refresh-length stall was ever seen");

    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    delete d;
    return fails ? 1 : 0;
}

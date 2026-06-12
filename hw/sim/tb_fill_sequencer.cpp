// Verilator testbench for fill_sequencer (gen2 L1 line-fill sequencer).
//
// Drives the upstream port as the granted L1 back side (level-held
// requests, dropped the cycle after completion) and models the L2's
// CPU-facing handshake downstream:
//   - cached read (re && cacheable): stage-0 busy cycle while the address
//     latches, stage-1 busy-low cycle with data — the real L2's II=2
//     admission (a new address is latched only once stage-1 has drained)
//   - pass-through (uncacheable read, any write): busy for a configurable
//     number of cycles, then completion
//
// Checks:
//   - pass-through transparency for uncacheable reads and for writes
//     (shape forwarded verbatim, busy/data mirrored, no fill activity)
//   - a line request engages without leaking a single-beat read downstream,
//     streams exactly LINE_WORDS cacheable word reads in ascending order,
//     forwards each beat to the fill port with the right index and data,
//     and raises fill_done coincident with the last beat
//   - upstream busy holds from the request cycle through fill_done
//   - a slow word mid-stream (the L2 missing internally) just stretches
//     the stream — no spurious beats while busy
//   - back-to-back transactions: a write the cycle after fill_done
//
// The module's SVA assertions (request held mid-stream, no consecutive
// beats, walk stays in the line) run under --assert throughout.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vfill_sequencer.h"
#include "verilated.h"

static const int LINE_WORDS = 4;

static Vfill_sequencer* dut;
static int errors = 0, tests = 0;

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp);
        errors++;
    }
}

static uint32_t word_at(uint32_t addr) { return addr ^ 0xFFFFFFFFu; }

// ── L2 model ────────────────────────────────────────────────────
static bool     s1_valid = false;
static uint32_t s1_addr = 0;
static int      s1_extra = 0;        // extra busy cycles for the latched word
static int      slow_word_stall = 0; // inject stall when this word latches (one-shot)
static int      slow_word_idx = -1;
static int      sb_lat = 1, sb_wait = 0;
static bool     sb_pending = false;
static int      sb_reads = 0, sb_writes = 0;
static uint32_t last_sb_addr = 0, last_sb_wdata = 0;
static uint8_t  last_sb_be = 0;
static bool     last_sb_cacheable = false;
static std::vector<uint32_t> latched_addrs;   // cached-read admission order

// Drives i_l2_* for the current cycle from o_l2_*; the companion edge
// function below commits the model state at the clock.
static void l2_drive() {
    dut->i_l2_busy = 0;
    if (dut->o_l2_re && dut->o_l2_cacheable) {
        if (s1_valid && s1_extra == 0) {
            dut->i_l2_busy  = 0;                  // stage-1: serve
            dut->i_l2_rdata = word_at(s1_addr);
        } else {
            dut->i_l2_busy = 1;                   // stage-0 / internal miss
        }
    } else if (dut->o_l2_re || dut->o_l2_we) {
        if (!sb_pending) { sb_pending = true; sb_wait = sb_lat; }
        if (sb_wait > 0) {
            dut->i_l2_busy = 1;
        } else {
            dut->i_l2_busy = 0;
            if (dut->o_l2_re) { dut->i_l2_rdata = word_at(dut->o_l2_addr); sb_reads++; }
            else              sb_writes++;
            last_sb_addr      = dut->o_l2_addr;
            last_sb_wdata     = dut->o_l2_wdata;
            last_sb_be        = dut->o_l2_byte_en;
            last_sb_cacheable = dut->o_l2_cacheable;
        }
    }
}

static void l2_edge() {
    if (dut->o_l2_re && dut->o_l2_cacheable) {
        if (s1_valid) {
            if (s1_extra > 0) s1_extra--;
            else              s1_valid = false;   // served this cycle
        } else {
            s1_valid = true;                      // stage-0 latch
            s1_addr  = dut->o_l2_addr;
            latched_addrs.push_back(dut->o_l2_addr);
            if ((int)((dut->o_l2_addr >> 2) & (LINE_WORDS - 1)) == slow_word_idx) {
                s1_extra = slow_word_stall;       // L2 misses internally
                slow_word_idx = -1;
            }
        }
    } else if (dut->o_l2_re || dut->o_l2_we) {
        if (sb_pending && sb_wait > 0) sb_wait--;
        else                           sb_pending = false;
    } else {
        sb_pending = false;
    }
}

// ── Clocking ────────────────────────────────────────────────────
static void cycle_eval() {
    dut->eval();
    l2_drive();
    dut->eval();
}
static void clock_edge() {
    l2_edge();
    dut->i_clk = 1; dut->eval();
    dut->i_clk = 0; dut->eval();
}

static void clear_upstream() {
    dut->i_addr = 0; dut->i_wdata = 0; dut->i_byte_en = 0;
    dut->i_re = 0; dut->i_we = 0; dut->i_cacheable = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vfill_sequencer;

    clear_upstream();
    dut->i_clk = 0;
    dut->i_rst = 1;
    cycle_eval(); clock_edge();
    cycle_eval(); clock_edge();
    dut->i_rst = 0;
    cycle_eval(); clock_edge();

    // ── Pass-through: uncacheable read ──────────────────────────
    dut->i_addr = 0x9004; dut->i_re = 1; dut->i_cacheable = 0;
    int waited = 0;
    for (int i = 0; i < 20 && (cycle_eval(), dut->o_busy); i++) {
        check("unc_read_no_fill", dut->o_fill_we | dut->o_fill_done, 0);
        clock_edge(); waited++;
    }
    check("unc_read_data", dut->o_rdata, word_at(0x9004));
    check("unc_read_waited", waited, sb_lat);
    check("unc_read_sb", sb_reads, 1);
    check("unc_read_cacheable_clear", last_sb_cacheable, 0);
    clock_edge();
    clear_upstream(); cycle_eval(); clock_edge();

    // ── Pass-through: cacheable write keeps its shape ────────────
    dut->i_addr = 0x9108; dut->i_wdata = 0xA5A5A5A5; dut->i_byte_en = 0xC;
    dut->i_we = 1; dut->i_cacheable = 1;
    for (int i = 0; i < 20 && (cycle_eval(), dut->o_busy); i++) clock_edge();
    check("wr_sb", sb_writes, 1);
    check("wr_addr", last_sb_addr, 0x9108);
    check("wr_wdata", last_sb_wdata, 0xA5A5A5A5);
    check("wr_be", last_sb_be, 0xC);
    check("wr_cacheable_kept", last_sb_cacheable, 1);
    clock_edge();
    clear_upstream(); cycle_eval(); clock_edge();

    // ── Line request: engage, stream, fill_done ──────────────────
    latched_addrs.clear();
    uint32_t base = 0x4D20;                       // line-aligned
    dut->i_addr = base; dut->i_re = 1; dut->i_cacheable = 1;

    int  beats = 0;
    bool done_seen = false, done_with_last_beat = false;
    uint32_t got_words[LINE_WORDS] = {0};
    bool got_seen[LINE_WORDS] = {false};
    int sb_r_before = sb_reads;

    for (int i = 0; i < 60 && !done_seen; i++) {
        cycle_eval();
        check("line_busy_held", dut->o_busy, 1);  // every cycle up to done
        if (dut->o_fill_we) {
            int w = dut->o_fill_word;
            check("line_word_in_range", w < LINE_WORDS, 1);
            check("line_word_order", (uint32_t)w, (uint32_t)beats);
            check("line_word_fresh", got_seen[w], 0);
            got_seen[w]  = true;
            got_words[w] = dut->o_fill_wdata;
            beats++;
        }
        if (dut->o_fill_done) {
            done_seen = true;
            done_with_last_beat = dut->o_fill_we && (dut->o_fill_word == LINE_WORDS - 1);
        }
        clock_edge();
    }
    check("line_done_seen", done_seen, 1);
    check("line_beats", beats, LINE_WORDS);
    check("line_done_coincides_last_beat", done_with_last_beat, 1);
    for (int w = 0; w < LINE_WORDS; w++) {
        char name[32];
        snprintf(name, sizeof(name), "line_data_w%d", w);
        check(name, got_words[w], word_at(base + 4 * w));
    }
    // The L2 saw exactly the line's words, in ascending order, no extras.
    check("line_l2_latches", (uint32_t)latched_addrs.size(), LINE_WORDS);
    for (int w = 0; w < (int)latched_addrs.size() && w < LINE_WORDS; w++) {
        char name[32];
        snprintf(name, sizeof(name), "line_l2_addr_w%d", w);
        check(name, latched_addrs[w], base + 4 * w);
    }
    check("line_no_sb_leak", sb_reads, sb_r_before);  // never a single-beat read

    // The L1 drops the request the cycle after fill_done.
    clear_upstream(); cycle_eval(); clock_edge();

    // ── Back-to-back: a write immediately after the fill ─────────
    dut->i_addr = 0x9200; dut->i_wdata = 0x0BADF00D; dut->i_byte_en = 0xF;
    dut->i_we = 1; dut->i_cacheable = 1;
    for (int i = 0; i < 20 && (cycle_eval(), dut->o_busy); i++) clock_edge();
    check("b2b_write_sb", sb_writes, 2);
    check("b2b_write_addr", last_sb_addr, 0x9200);
    clock_edge();
    clear_upstream(); cycle_eval(); clock_edge();

    // ── Slow word mid-stream (L2 misses internally on word 2) ────
    latched_addrs.clear();
    slow_word_idx = 2; slow_word_stall = 8;
    base = 0x5AC0;
    dut->i_addr = base; dut->i_re = 1; dut->i_cacheable = 1;
    beats = 0; done_seen = false;
    int total_cycles = 0;
    for (int i = 0; i < 100 && !done_seen; i++) {
        cycle_eval();
        if (dut->o_fill_we) beats++;
        if (dut->o_fill_done) done_seen = true;
        clock_edge(); total_cycles++;
    }
    check("slow_done", done_seen, 1);
    check("slow_beats", beats, LINE_WORDS);
    check("slow_stretched", total_cycles > 2 * LINE_WORDS + 8, 1);
    clear_upstream(); cycle_eval(); clock_edge();

    printf("fill_sequencer: %d tests, %d errors\n", tests, errors);
    dut->final();
    delete dut;
    return errors ? 1 : 0;
}

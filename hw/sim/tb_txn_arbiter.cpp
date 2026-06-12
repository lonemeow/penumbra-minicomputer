// Verilator testbench for txn_arbiter (gen2 transactional I/D arbiter).
//
// Drives both L1 back-side ports as honest masters (level-held requests,
// dropped the cycle after completion) and models the downstream side (the
// fill sequencer's upstream port): single beats follow the busy handshake
// with configurable latency; a line request (re && cacheable) holds busy
// high and streams fill beats with fill_done on the last one — including
// an injectable one-cycle busy-low glitch mid-stream to prove completion
// is type-dependent, not busy-driven.
//
// Checks:
//   - idle: nothing forwarded
//   - D-priority on simultaneous pending; the loser waits behind busy=1
//     and is granted the cycle after the winner completes (no dead cycle)
//   - the grant locks across a multi-cycle single beat: a request arriving
//     mid-transaction cannot steal the port
//   - a single beat that completes on its first cycle never takes the
//     lock (a dangling lock would deadlock the next grant)
//   - line transactions: fill strobes reach only the owner, word/wdata
//     broadcast, the non-owner waits, and a mid-stream busy-drop does NOT
//     release the grant — only fill_done does
//   - full-shape forwarding (wdata/byte_en/cacheable verbatim)
//
// The module's SVA assertions (lock taken/released/stable, fill activity
// only inside a line transaction) run under --assert throughout.

#include <cstdio>
#include <cstdint>
#include "Vtxn_arbiter.h"
#include "verilated.h"

static const int LINE_WORDS = 4;

static Vtxn_arbiter* dut;
static int errors = 0, tests = 0;

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp);
        errors++;
    }
}

// ── Downstream model (the fill sequencer's upstream face) ───────
static int  m_lat = 1, m_wait = 0;
static bool m_pending = false;
static int  m_reads = 0, m_writes = 0;
static uint32_t last_addr = 0, last_wdata = 0;
static uint8_t  last_be = 0;
static bool     last_cacheable = false;

static bool fill_active = false;
static int  fill_beat = 0, fill_gap = 0;
static int  fills_done = 0;
static bool glitch_busy = false;      // one-shot: force busy low mid-stream

static void ds_drive() {
    dut->i_fill_we = 0; dut->i_fill_done = 0;
    dut->i_m_busy = 0;

    if (fill_active) {
        dut->i_m_busy = 1;                        // sequencer holds busy on lines
        if (glitch_busy) { dut->i_m_busy = 0; glitch_busy = false; return; }
        if (fill_gap > 0) { fill_gap--; return; }
        dut->i_fill_we    = 1;
        dut->i_fill_word  = fill_beat;
        dut->i_fill_wdata = 0xF1110000u + (uint32_t)fill_beat;
        if (fill_beat == LINE_WORDS - 1) {
            dut->i_fill_done = 1;
            fill_active = false;
            fills_done++;
        } else {
            fill_beat++;
            fill_gap = 1;
        }
        return;
    }

    if (dut->o_m_re && dut->o_m_cacheable) {      // line request: engage
        fill_active = true;
        fill_beat = 0;
        fill_gap = 2;
        last_addr = dut->o_m_addr;
        dut->i_m_busy = 1;
        return;
    }

    if (dut->o_m_re || dut->o_m_we) {             // single beat
        if (!m_pending) { m_pending = true; m_wait = m_lat; }
        if (m_wait > 0) {
            dut->i_m_busy = 1;
        } else {
            dut->i_m_busy = 0;
            if (dut->o_m_re) { dut->i_m_rdata = dut->o_m_addr ^ 0xFFFFFFFFu; m_reads++; }
            else             m_writes++;
            last_addr      = dut->o_m_addr;
            last_wdata     = dut->o_m_wdata;
            last_be        = dut->o_m_byte_en;
            last_cacheable = dut->o_m_cacheable;
        }
    }
}

static void ds_edge() {
    if (!fill_active && (dut->o_m_re || dut->o_m_we)
        && !(dut->o_m_re && dut->o_m_cacheable)) {
        if (m_pending && m_wait > 0) m_wait--;
        else                         m_pending = false;
    } else if (!(dut->o_m_re || dut->o_m_we)) {
        m_pending = false;
    }
}

// ── Clocking ────────────────────────────────────────────────────
static void cycle_eval() {
    dut->eval();
    ds_drive();
    dut->eval();
}
static void clock_edge() {
    ds_edge();
    dut->i_clk = 1; dut->eval();
    dut->i_clk = 0; dut->eval();
}

static void clear_i() {
    dut->i_i_addr = 0; dut->i_i_wdata = 0; dut->i_i_byte_en = 0;
    dut->i_i_re = 0; dut->i_i_we = 0; dut->i_i_cacheable = 0;
}
static void clear_d() {
    dut->i_d_addr = 0; dut->i_d_wdata = 0; dut->i_d_byte_en = 0;
    dut->i_d_re = 0; dut->i_d_we = 0; dut->i_d_cacheable = 0;
}
static void idle_cycle() { cycle_eval(); clock_edge(); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vtxn_arbiter;

    clear_i(); clear_d();
    dut->i_m_busy = 0; dut->i_m_rdata = 0;
    dut->i_fill_we = 0; dut->i_fill_word = 0; dut->i_fill_wdata = 0;
    dut->i_fill_done = 0;
    dut->i_clk = 0;
    dut->i_rst = 1;
    idle_cycle(); idle_cycle();
    dut->i_rst = 0;
    idle_cycle();

    // ── Idle: nothing forwarded ──────────────────────────────────
    cycle_eval();
    check("idle_no_re", dut->o_m_re, 0);
    check("idle_no_we", dut->o_m_we, 0);
    clock_edge();

    // ── D-priority tiebreak + no-dead-cycle handover ─────────────
    dut->i_i_addr = 0x1000; dut->i_i_re = 1;     // uncacheable read
    dut->i_d_addr = 0x2000; dut->i_d_re = 1;
    cycle_eval();
    check("tie_d_wins_addr", dut->o_m_addr, 0x2000);
    check("tie_i_waits", dut->o_i_busy, 1);
    check("tie_d_busy_mirrors", dut->o_d_busy, dut->i_m_busy);
    clock_edge();
    // D's wait: run until its busy drops, then D deasserts.
    for (int i = 0; i < 10; i++) {
        cycle_eval();
        if (!dut->o_d_busy) break;
        check("tie_fwd_stays_d", dut->o_m_addr, 0x2000);
        clock_edge();
    }
    check("tie_d_data", dut->o_d_rdata, 0x2000 ^ 0xFFFFFFFFu);
    clock_edge();
    clear_d();
    // Very next cycle: the waiting I side owns the port.
    cycle_eval();
    check("tie_handover_to_i", dut->o_m_addr, 0x1000);
    check("tie_i_busy_mirrors", dut->o_i_busy, dut->i_m_busy);
    for (int i = 0; i < 10 && (cycle_eval(), dut->o_i_busy); i++) clock_edge();
    check("tie_i_data", dut->o_i_rdata, 0x1000 ^ 0xFFFFFFFFu);
    clock_edge();
    clear_i(); idle_cycle();

    // ── Lock across a multi-cycle write: no mid-transaction steal ──
    m_lat = 3;
    dut->i_d_addr = 0x3000; dut->i_d_wdata = 0xC001D00D;
    dut->i_d_byte_en = 0x3; dut->i_d_we = 1; dut->i_d_cacheable = 1;
    cycle_eval();
    check("lock_d_first", dut->o_m_addr, 0x3000);
    clock_edge();
    dut->i_i_addr = 0x1100; dut->i_i_re = 1;     // arrives mid-transaction
    for (int i = 0; i < 10; i++) {
        cycle_eval();
        if (!dut->o_d_busy) break;
        check("lock_no_steal", dut->o_m_addr, 0x3000);
        check("lock_i_waits", dut->o_i_busy, 1);
        clock_edge();
    }
    check("lock_wr_wdata", last_wdata, 0xC001D00D);
    check("lock_wr_be", last_be, 0x3);
    check("lock_wr_cacheable", last_cacheable, 1);
    clock_edge();
    clear_d();
    cycle_eval();
    check("lock_then_i", dut->o_m_addr, 0x1100);
    for (int i = 0; i < 10 && (cycle_eval(), dut->o_i_busy); i++) clock_edge();
    clock_edge();
    clear_i(); idle_cycle();

    // ── Same-cycle completion never takes the lock ───────────────
    m_lat = 0;
    dut->i_i_addr = 0x1200; dut->i_i_re = 1;
    cycle_eval();
    check("instant_i_done", dut->o_i_busy, 0);
    check("instant_i_data", dut->o_i_rdata, 0x1200 ^ 0xFFFFFFFFu);
    clock_edge();
    clear_i(); idle_cycle();
    // If the lock dangled to I, this D request would never be forwarded.
    dut->i_d_addr = 0x3300; dut->i_d_re = 1;
    cycle_eval();
    check("instant_no_dangle", dut->o_m_addr, 0x3300);
    check("instant_d_done", dut->o_d_busy, 0);
    clock_edge();
    clear_d(); idle_cycle();
    m_lat = 1;

    // ── Line transaction on I: routing, isolation, type-dependent
    //    completion (a mid-stream busy-drop must not release) ─────
    dut->i_i_addr = 0x4D40; dut->i_i_re = 1; dut->i_i_cacheable = 1;
    int i_beats = 0, d_beats = 0;
    bool done_i = false, glitched = false;
    for (int c = 0; c < 60 && !done_i; c++) {
        if (i_beats == 2 && !glitched) { glitch_busy = true; glitched = true; }
        if (c == 3) { dut->i_d_addr = 0x3400; dut->i_d_re = 1; }   // mid-line arrival
        cycle_eval();
        check("line_fwd_is_i", dut->o_m_addr, 0x4D40);
        if (dut->i_d_re) check("line_d_waits", dut->o_d_busy, 1);
        if (dut->o_i_fill_we) {
            check("line_word_seq", dut->o_i_fill_word, (uint32_t)i_beats);
            check("line_wdata", dut->o_i_fill_wdata, 0xF1110000u + (uint32_t)i_beats);
            i_beats++;
        }
        if (dut->o_d_fill_we) d_beats++;
        if (dut->o_i_fill_done) done_i = true;
        check("line_done_not_leaked", dut->o_d_fill_done, 0);
        clock_edge();
    }
    check("line_done", done_i, 1);
    check("line_i_beats", i_beats, LINE_WORDS);
    check("line_d_isolated", d_beats, 0);
    check("line_glitch_injected", glitched, 1);
    // The L1 leaves S_FILL at the fill_done edge, so its request is low
    // during the very next cycle — model that honestly, no extra edge.
    clear_i();
    // Handover: D's held request owns the port the cycle after fill_done.
    cycle_eval();
    check("line_handover_to_d", dut->o_m_addr, 0x3400);
    for (int i = 0; i < 10 && (cycle_eval(), dut->o_d_busy); i++) clock_edge();
    clock_edge();
    clear_d(); idle_cycle();

    // ── Line transaction on D: fill strobes route to D only ──────
    dut->i_d_addr = 0x5E80; dut->i_d_re = 1; dut->i_d_cacheable = 1;
    i_beats = 0; d_beats = 0;
    bool done_d = false;
    for (int c = 0; c < 60 && !done_d; c++) {
        cycle_eval();
        if (dut->o_d_fill_we) d_beats++;
        if (dut->o_i_fill_we) i_beats++;
        if (dut->o_d_fill_done) done_d = true;
        clock_edge();
    }
    check("dline_done", done_d, 1);
    check("dline_d_beats", d_beats, LINE_WORDS);
    check("dline_i_isolated", i_beats, 0);
    clear_d(); idle_cycle();

    check("fills_total", fills_done, 2);

    printf("txn_arbiter: %d tests, %d errors\n", tests, errors);
    dut->final();
    delete dut;
    return errors ? 1 : 0;
}

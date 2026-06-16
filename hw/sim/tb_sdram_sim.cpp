// Full-stack SDRAM consistency testbench.
//
// Drives the complete sim SDRAM stack (sdram_bus_adapter + sdram_cdc +
// sdram_ctrl + sdram_phy_sim + sdram_model) through its cache-facing
// bus and checks the one end-to-end invariant: a read returns the value
// most recently written to that address.  A C++ shadow of every
// committed write is ground truth.
//
// Why here (not in tb_sdram_adapter): the adapter's speculative addr+4
// prefetch returned another transaction's word when a non-matching real
// read overlapped a still-in-flight spec.  It only reproduces with the
// real CDC + controller timing — the adapter unit test's in-order mock
// cannot create it.
//
// Trigger shape (matches what reaches sdram_sim in the gen2 system):
// the fill_sequencer issues a GAPLESS sequential line fill, whose last
// word queues a fresh full-latency prefetch for the word just past the
// line; the uncached vecfetch read then arrives as the NEXT, GAPPED
// transaction and does not match that prefetch — so it issues a real
// request while the fresh spec is still in flight, the overlap that
// transposes the two responses.  We reproduce exactly that: drive a
// gapless run (no data check — just advance the FSM and prime the
// fresh spec), then issue a gapped, checked read of an unrelated word.
//
// Clocking matches tb_penumbra2_prog.cpp (4 SDRAM cycles per CPU cycle).

#include <cstdio>
#include <cstdint>
#include "Vsdram_sim.h"

static int errors = 0;

static void tick(Vsdram_sim* d) {
    d->i_clk = 0; d->eval();
    for (int s = 0; s < 4; s++) { d->i_sdram_clk = !d->i_sdram_clk; d->eval(); }
    d->i_clk = 1; d->eval();
    for (int s = 0; s < 4; s++) { d->i_sdram_clk = !d->i_sdram_clk; d->eval(); }
}

static void reset(Vsdram_sim* d) {
    d->i_rst = 1; d->i_sdram_clk = 0;
    d->i_addr = 0; d->i_wdata = 0; d->i_byte_en = 0xF;
    d->i_re = 0; d->i_we = 0;
    tick(d); tick(d);
    d->i_rst = 0;
    for (int i = 0; i < 400; i++) tick(d);
}

static bool do_write(Vsdram_sim* d, uint32_t addr, uint32_t wdata, uint8_t be) {
    d->i_addr = addr; d->i_wdata = wdata; d->i_byte_en = be;
    d->i_we = 1; d->i_re = 0; d->eval();
    int w = 0;
    while (d->o_busy) {
        tick(d);
        if (++w > 4000) { printf("  [FAIL] write @0x%08X stuck\n", addr); errors++; d->i_we = 0; return false; }
    }
    d->i_we = 0; tick(d);
    return true;
}

// ── Ground-truth shadow ──
static constexpr int NWORDS = 4096;          // 16 KiB — several SDRAM rows
static uint32_t shadow[NWORDS];

static uint32_t lcg = 0x12345678u;
static uint32_t rnd() { lcg = lcg * 1664525u + 1013904223u; return lcg; }

// Drive a GAPLESS sequential run (i_re held the whole way — the
// PRESENT->BEGIN fast path, exactly how fill_sequencer streams a line).
// No data check: the point is to advance the FSM and leave a *fresh*,
// full-latency prefetch for (start+len) in flight when i_re drops.
// Sampling-proof advance: wait for o_busy high (started) then low
// (done) per word.
static void drive_run(Vsdram_sim* d, int start, int len) {
    int waited = 0;
    for (int k = 0; k < len; k++) {
        d->i_addr = (uint32_t)(start + k) * 4u;
        d->i_re = 1; d->i_we = 0;
        d->eval();
        bool seen = false;
        for (;;) {
            tick(d);
            if (d->o_busy) seen = true;
            else if (seen) break;
            if (++waited > 12000) { printf("  [FAIL] run stalled @word %d\n", start + k); errors++; d->i_re = 0; return; }
        }
    }
    d->i_re = 0; tick(d);     // drop i_re: fresh spec for (start+len) now in flight
}

// Gapped single read + shadow check (arbiter-style, from IDLE — the
// uncached vecfetch's traversal).  This is the read that overlaps the
// fresh spec; if its response is transposed, it returns the wrong word.
static void read_check(Vsdram_sim* d, int word) {
    d->i_addr = (uint32_t)word * 4u; d->i_re = 1; d->i_we = 0; d->eval();
    int w = 0;
    while (d->o_busy) {
        tick(d);
        if (++w > 12000) { printf("  [FAIL] read @word %d stuck\n", word); errors++; d->i_re = 0; return; }
    }
    uint32_t got = d->o_rdata;
    if (got != shadow[word]) {
        printf("  [FAIL] read @0x%08X got 0x%08X want 0x%08X\n",
               (unsigned)word * 4u, got, shadow[word]);
        errors++;
    }
    d->i_re = 0; tick(d);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_sim* d = new Vsdram_sim;
    printf("== sdram_sim full-stack consistency test ==\n");

    reset(d);

    for (int i = 0; i < NWORDS && errors == 0; i++) {
        uint32_t v = 0xC0DE0000u | (uint32_t)i;
        if (!do_write(d, (uint32_t)i * 4u, v, 0xF)) break;
        shadow[i] = v;
    }
    printf("  seeded %d words\n", NWORDS);

    // Repeatedly: gapless fill (priming a fresh spec) → gapped read of
    // an unrelated word (overlapping it).  Vary run length and the
    // (line, jump) addresses across rows so the spec/response timing
    // sweeps alignments; rewrite occasionally so a stale return differs.
    const int ITERS = 40000;
    int reads = 0;
    for (int it = 0; it < ITERS && errors == 0; it++) {
        uint32_t r = rnd();
        int start = (int)(r % (uint32_t)(NWORDS - 8));
        int len   = 3 + (int)((r >> 20) % 4);             // 3..6 line words
        drive_run(d, start, len);

        int jump = (int)((rnd() >> 5) % (uint32_t)NWORDS); // unrelated word
        read_check(d, jump);
        reads++;

        if ((rnd() & 7) == 0) {
            int i = (int)((rnd() >> 8) % (uint32_t)NWORDS);
            uint32_t v = 0xBEEF0000u | (uint32_t)(it & 0xFFFF);
            if (!do_write(d, (uint32_t)i * 4u, v, 0xF)) break;
            shadow[i] = v;
        }
    }
    printf("  %d checked reads over %d fill+jump iters\n", reads, ITERS);

    delete d;
    if (errors == 0) { printf("== ALL PASS ==\n"); return 0; }
    printf("== %d FAILURE(S) ==\n", errors);
    return 1;
}

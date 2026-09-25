// Line-buffer SDRAM stack testbench.
//
// Drives sdram_sim built with the line-buffer fileset (the adapter and
// stack from hw/rtl/io/sdram_line/, so the controller runs an
// eight-beat burst and the adapter holds the line it returns).
//
// Tests what the line buffer adds rather than repeating the word-at-a-
// time suite in tb_sdram_sim:
//   • A read of a held line is answered without a controller round
//     trip, so it costs far fewer cycles than the miss that filled it.
//   • Every word of the line reads back correctly, not just the one
//     that missed.
//   • A write inside the held line does not leave a stale copy behind.
//   • Sub-word writes reach the right lane through the line path.
//   • Holding one line means the previous one is gone.
//
// Clocking matches tb_sdram_sim.cpp (4 SDRAM cycles per CPU cycle).

#include <cstdio>
#include <cstdint>
#include "Vsdram_sim.h"

static const uint32_t LINE_BYTES = 16;
static const int      LINE_WORDS = 4;

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

static void do_write(Vsdram_sim* d, uint32_t addr, uint32_t wdata, uint8_t be = 0xF) {
    d->i_addr = addr; d->i_wdata = wdata; d->i_byte_en = be;
    d->i_we = 1; d->i_re = 0; d->eval();
    int w = 0;
    do {
        tick(d);
        if (++w > 4000) { printf("  FAIL: write @0x%08X stuck\n", addr); errors++; break; }
    } while (!d->o_ack);
    tick(d);                      // ack cycle, strobe held
    d->i_we = 0; tick(d);
}

// Returns the word, and through `cycles` how long the access took —
// which is what distinguishes a buffered hit from a controller miss.
static uint32_t do_read(Vsdram_sim* d, uint32_t addr, int* cycles = nullptr) {
    d->i_addr = addr; d->i_byte_en = 0xF;
    d->i_re = 1; d->i_we = 0; d->eval();
    int w = 0;
    do {
        tick(d);
        if (++w > 4000) { printf("  FAIL: read @0x%08X stuck\n", addr); errors++; break; }
    } while (!d->o_ack);
    uint32_t v = d->o_rdata;
    if (cycles) *cycles = w;
    tick(d);
    d->i_re = 0; tick(d);
    return v;
}

static void check(uint32_t got, uint32_t want, const char* label) {
    if (got != want) {
        printf("  FAIL: %s — got 0x%08X want 0x%08X\n", label, got, want);
        errors++;
    } else {
        printf("  PASS: %s\n", label);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_sim* d = new Vsdram_sim;

    printf("── SDRAM line-buffer stack ──\n");
    reset(d);

    const uint32_t A = 0x00001000;      // line-aligned
    const uint32_t B = 0x00002000;      // a different line

    // ── Fill a line's worth of words ─────────────────────────
    for (int w = 0; w < LINE_WORDS; w++)
        do_write(d, A + 4 * w, 0xC0DE0000u + w);

    // ── The first read misses, the rest of the line hits ─────
    int miss_cyc = 0, hit_cyc[LINE_WORDS] = {0};
    check(do_read(d, A, &miss_cyc), 0xC0DE0000u, "line word 0 (miss)");
    for (int w = 1; w < LINE_WORDS; w++) {
        char label[64];
        snprintf(label, sizeof(label), "line word %d (hit)", w);
        check(do_read(d, A + 4 * w, &hit_cyc[w]), 0xC0DE0000u + w, label);
    }

    printf("  miss %d cycles; hits %d %d %d\n",
           miss_cyc, hit_cyc[1], hit_cyc[2], hit_cyc[3]);
    for (int w = 1; w < LINE_WORDS; w++) {
        if (hit_cyc[w] >= miss_cyc) {
            printf("  FAIL: word %d cost %d cycles, not cheaper than the %d-cycle miss\n",
                   w, hit_cyc[w], miss_cyc);
            errors++;
        }
    }
    if (errors == 0) printf("  PASS: held line answers without a controller round trip\n");

    // ── A write inside the held line leaves no stale copy ────
    do_write(d, A + 4, 0xFEEDFACEu);
    check(do_read(d, A + 4), 0xFEEDFACEu, "write into held line is visible");
    check(do_read(d, A),     0xC0DE0000u, "neighbour word survives that write");

    // ── Sub-word write through the line path ─────────────────
    do_write(d, A + 8, 0x000000AAu, 0x1);          // low byte only
    check(do_read(d, A + 8), 0xC0DE00AAu, "byte enable reaches one lane");

    // ── Holding one line means the previous one is gone ──────
    for (int w = 0; w < LINE_WORDS; w++)
        do_write(d, B + 4 * w, 0xBEEF0000u + w);

    check(do_read(d, B), 0xBEEF0000u, "second line reads back");
    int refill_cyc = 0;
    check(do_read(d, A, &refill_cyc), 0xC0DE0000u, "first line still correct after eviction");
    if (refill_cyc < miss_cyc / 2) {
        printf("  FAIL: re-read of evicted line cost only %d cycles — it was not refetched\n",
               refill_cyc);
        errors++;
    } else {
        printf("  PASS: evicted line is refetched, not served stale\n");
    }

    delete d;
    if (errors == 0) { printf("── ALL PASS ──\n"); return 0; }
    printf("── %d FAILURE(S) ──\n", errors);
    return 1;
}

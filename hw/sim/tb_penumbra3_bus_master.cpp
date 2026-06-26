// Phase-0 probe P0.4: gen3 bus master vs the real SDRAM adapter.
//
// Drives penumbra3_bus_master's transaction interface against the full
// sim SDRAM stack (sdram_sim) and checks two things:
//
//   • No deadlock. The gen2 register slice gapped the read stream and
//     hung the adapter's speculative-prefetch FSM. The gen3 master holds
//     re/we across a burst, so the stream is gapless within a line; this
//     test stresses that against the real adapter, plus the end-of-line
//     dangling speculation the adapter must abandon when the next
//     transaction lands elsewhere. A stuck transaction fails on timeout.
//
//   • Correctness. A C++ shadow of every committed write is ground
//     truth; line fills, single-beat reads, line evictions, and sub-word
//     writes must all agree with it -- so a transposed spec response or a
//     mis-lane'd sub-word write is caught, not a coin-flip.
//
// Clocking: 4 SDRAM cycles per CPU cycle, matching tb_penumbra2_prog.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_bus_master_test.h"

static int errors = 0;

static constexpr int LINE_WORDS  = 4;
static constexpr int NWORDS      = 1024;     // 4 KiB -- several SDRAM rows
static constexpr int TXN_TIMEOUT = 20000;    // CPU cycles before a txn is "stuck"

static void tick(Vpenumbra3_bus_master_test* d) {
    d->i_clk = 0; d->eval();
    for (int s = 0; s < 4; s++) { d->i_sdram_clk = !d->i_sdram_clk; d->eval(); }
    d->i_clk = 1; d->eval();
    for (int s = 0; s < 4; s++) { d->i_sdram_clk = !d->i_sdram_clk; d->eval(); }
}

static void reset(Vpenumbra3_bus_master_test* d) {
    d->i_rst = 1; d->i_sdram_clk = 0;
    d->i_req_valid = 0; d->i_req_we = 0; d->i_req_line = 0;
    d->i_req_addr = 0; d->i_req_byte_en = 0xF;
    for (int k = 0; k < LINE_WORDS; k++) d->i_req_wline[k] = 0;
    tick(d); tick(d);
    d->i_rst = 0;
    for (int i = 0; i < 400; i++) tick(d);   // let the SDRAM init sequence finish
}

// Issue one transaction; return false on timeout (treated as a deadlock).
static bool do_txn(Vpenumbra3_bus_master_test* d, bool we, bool line,
                   uint32_t addr, uint8_t be,
                   const uint32_t* wdata, uint32_t* rdata) {
    int w = 0;
    while (!d->o_req_ready) {
        tick(d);
        if (++w > TXN_TIMEOUT) {
            printf("  [FAIL] master never ready (txn @0x%08X)\n", addr);
            errors++; return false;
        }
    }
    d->i_req_valid   = 1;
    d->i_req_we      = we;
    d->i_req_line    = line;
    d->i_req_addr    = addr;
    d->i_req_byte_en = be;
    for (int k = 0; k < LINE_WORDS; k++) d->i_req_wline[k] = wdata ? wdata[k] : 0;
    d->eval();
    tick(d);                         // acceptance edge (o_req_ready high)
    d->i_req_valid = 0; d->eval();

    w = 0;
    while (!d->o_rsp_valid) {
        tick(d);
        if (++w > TXN_TIMEOUT) {
            printf("  [FAIL] %s @0x%08X never completed (deadlock?)\n",
                   we ? "write" : "read", addr);
            errors++; return false;
        }
    }
    if (d->o_rsp_fault) { printf("  [FAIL] unexpected fault @0x%08X\n", addr); errors++; }
    if (rdata) for (int k = 0; k < LINE_WORDS; k++) rdata[k] = d->o_rsp_rline[k];
    tick(d);                         // M_DONE -> M_IDLE
    return true;
}

// ── Ground-truth shadow ──
static uint32_t shadow[NWORDS];

// xorshift32: all bits are well-mixed, so both the `& 3` decisions and the
// `% N` index picks below are uniform. (An LCG's low bits have tiny periods
// that phase-lock against the per-iteration call count and would silently
// starve the write paths.)
static uint32_t rng = 0x12345678u;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// Merge a sub-word write into the shadow per its byte-enable mask.
static uint32_t merge(uint32_t old_val, uint32_t val, uint8_t be) {
    uint32_t mask = 0;
    for (int b = 0; b < 4; b++) if (be & (1u << b)) mask |= 0xFFu << (b * 8);
    return (old_val & ~mask) | (val & mask);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vpenumbra3_bus_master_test* d = new Vpenumbra3_bus_master_test;
    printf("== penumbra3_bus_master P0.4: burst vs real SDRAM adapter ==\n");

    reset(d);

    // Seed every word via line-write (eviction-shaped) transactions.
    for (int lw = 0; lw < NWORDS / LINE_WORDS && errors == 0; lw++) {
        uint32_t wd[LINE_WORDS];
        for (int k = 0; k < LINE_WORDS; k++) {
            uint32_t word = (uint32_t)lw * LINE_WORDS + k;
            wd[k] = 0xC0DE0000u | word;
            shadow[word] = wd[k];
        }
        if (!do_txn(d, true, true, (uint32_t)lw * 16u, 0xF, wd, nullptr)) break;
    }
    printf("  seeded %d words via line writes\n", NWORDS);

    const int ITERS = 2000;
    int line_reads = 0, beat_reads = 0, line_writes = 0, subword_writes = 0;
    for (int it = 0; it < ITERS && errors == 0; it++) {
        // (1) Line read-fill -- primes an end-of-line addr+LINE speculation.
        uint32_t lbase = (rnd() % (uint32_t)(NWORDS / LINE_WORDS)) * LINE_WORDS;
        uint32_t rd[LINE_WORDS];
        if (!do_txn(d, false, true, lbase * 4u, 0xF, nullptr, rd)) break;
        for (int k = 0; k < LINE_WORDS; k++)
            if (rd[k] != shadow[lbase + k]) {
                printf("  [FAIL] line read word %u[+%d] got 0x%08X want 0x%08X\n",
                       lbase, k, rd[k], shadow[lbase + k]); errors++;
            }
        line_reads++;
        if (errors) break;

        // (2) Non-contiguous single-beat read -- the gapped access that
        //     overlaps the fresh end-of-line spec (the gen2 deadlock shape).
        uint32_t jw = rnd() % NWORDS;
        uint32_t br[LINE_WORDS];
        if (!do_txn(d, false, false, jw * 4u, 0xF, nullptr, br)) break;
        if (br[0] != shadow[jw]) {
            printf("  [FAIL] beat read word %u got 0x%08X want 0x%08X\n",
                   jw, br[0], shadow[jw]); errors++;
        }
        beat_reads++;
        if (errors) break;

        // (3) Occasional line write-back eviction.
        if ((rnd() & 3) == 0) {
            uint32_t wb = (rnd() % (uint32_t)(NWORDS / LINE_WORDS)) * LINE_WORDS;
            uint32_t wd[LINE_WORDS];
            for (int k = 0; k < LINE_WORDS; k++)
                wd[k] = 0xE71C0000u | (uint32_t)((it << 2) + k);
            if (!do_txn(d, true, true, wb * 4u, 0xF, wd, nullptr)) break;
            for (int k = 0; k < LINE_WORDS; k++) shadow[wb + k] = wd[k];
            line_writes++;
        }

        // (4) Occasional uncached sub-word write.
        if ((rnd() & 3) == 0) {
            uint32_t sw  = rnd() % NWORDS;
            uint8_t  be  = (uint8_t)(rnd() & 0xF); if (be == 0) be = 0xF;
            uint32_t val = rnd();
            uint32_t wd[LINE_WORDS] = { val, 0, 0, 0 };
            if (!do_txn(d, true, false, sw * 4u, be, wd, nullptr)) break;
            shadow[sw] = merge(shadow[sw], val, be);
            subword_writes++;
        }
    }
    printf("  %d line reads, %d beat reads, %d line writes, %d sub-word writes\n",
           line_reads, beat_reads, line_writes, subword_writes);

    delete d;
    if (errors == 0) { printf("== ALL PASS ==\n"); return 0; }
    printf("== %d FAILURE(S) ==\n", errors);
    return 1;
}

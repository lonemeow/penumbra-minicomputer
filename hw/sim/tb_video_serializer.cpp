// Verilator testbench for the Penumbra video 10:1 DDR serializer
// (hw/rtl/io/video/video_serializer.sv).
//
// Drives the pixel and serial clocks at the contractual 1:5 ratio and
// replays every possible phase offset between them — the module must
// lock regardless of where the pixel edge lands within the serial
// cycle. Words change just after pixel-clock edges, exactly like the
// pclk-registered TMDS encoder output upstream. The (o_d0, o_d1)
// stream, flattened d0-then-d1 (ODDRX1F emits D0 on the high half),
// must contain the fed word sequence LSB-first, back-to-back with no
// gaps, repeats, or slips, locking within the contractual five words
// of reset release; a mid-run reset must re-lock the same way. The
// module's own load_cadence assertion is armed via --assert.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vvideo_serializer.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static Vvideo_serializer* dut;

static uint32_t rng_state = 1;
static uint32_t rng() {   // xorshift32 — deterministic across platforms
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// One serial-clock cycle. ph is this cycle's position 0..4 within the
// pixel period; the pixel posedge fires when ph == offset, coincident
// with the serial posedge the way the same-PLL clocks are on the
// board (a coincident-edge sample sees the pre-edge value — the
// capture lands on the next serial edge, as static timing would have
// it). feed, when given, is applied just after the pixel edge, the
// way a pclk-registered upstream output changes.
static void serial_cycle(int ph, int offset, const uint16_t* feed,
                         std::vector<uint8_t>* bits) {
    dut->i_sclk = 0;
    if (ph == (offset + 3) % 5) dut->i_pclk = 0;   // falling edge placement is arbitrary
    dut->eval();
    dut->i_sclk = 1;
    if (ph == offset) dut->i_pclk = 1;
    dut->eval();
    if (ph == offset && feed) { dut->i_word = *feed; dut->eval(); }
    if (bits) {
        bits->push_back((uint8_t)dut->o_d0);
        bits->push_back((uint8_t)dut->o_d1);
    }
}

// Reset spanning both domains: held across two-plus pixel periods
// (the testbench convention's two cycles, in the slower clock).
static void reset_dut(int offset) {
    dut->i_rst  = 1;
    dut->i_word = 0;
    for (int c = 0; c < 12; c++) serial_cycle(c % 5, offset, nullptr, nullptr);
    dut->i_rst = 0;
}

// Feed the word list, collecting the flattened DDR stream; then run a
// few idle pixel periods so the tail words drain into it.
static std::vector<uint8_t> run_words(int offset,
                                      const std::vector<uint16_t>& words) {
    std::vector<uint8_t> bits;
    for (uint16_t w : words)
        for (int ph = 0; ph < 5; ph++)
            serial_cycle(ph, offset, &w, &bits);
    for (int c = 0; c < 20; c++) serial_cycle(c % 5, offset, nullptr, &bits);
    return bits;
}

// Locate the fed sequence in the stream (lock may discard a bounded
// prefix and the first latched words), then require an exact match to
// the end — any gap, repeat, or slip breaks the alignment.
static void check_stream(const std::vector<uint8_t>& bits,
                         const std::vector<uint16_t>& fed,
                         const char* tag) {
    std::vector<uint8_t> F;
    for (uint16_t w : fed)
        for (int b = 0; b < 10; b++) F.push_back((uint8_t)((w >> b) & 1));

    const int LOCK_BITS = 50;   // five words — the contractual lock bound
    int found_s = -1, found_k = -1;
    for (int k = 0; k <= 3 && found_s < 0; k++) {
        for (int s = 0; s <= LOCK_BITS && found_s < 0; s++) {
            bool ok = true;      // 30 matching bits make a credible lock
            for (int i = 0; i < 30 && ok; i++)
                ok = (size_t)(s + i) < bits.size()
                  && (size_t)(10 * k + i) < F.size()
                  && bits[s + i] == F[10 * k + i];
            if (ok) { found_s = s; found_k = k; }
        }
    }
    CHECK(found_s >= 0, "%s: no lock within %d bits of reset", tag, LOCK_BITS);
    if (found_s < 0) return;

    long n = 0, mismatches = 0, first_bad = -1;
    while ((size_t)(found_s + n) < bits.size()
        && (size_t)(10 * found_k + n) < F.size()) {
        if (bits[found_s + n] != F[10 * found_k + n]) {
            mismatches++;
            if (first_bad < 0) first_bad = n;
        }
        n++;
    }
    CHECK(mismatches == 0,
          "%s: %ld/%ld bits wrong after lock (first at bit %ld, word %ld)",
          tag, mismatches, n, first_bad, first_bad / 10);
}

int main() {
    for (int offset = 0; offset < 5; offset++) {
        dut = new Vvideo_serializer;
        reset_dut(offset);

        // Run A: random lead-in (distinct words keep the lock search
        // unambiguous), then the directed set — the DVI clock-lane
        // pattern, the four control codes, and the corner words.
        std::vector<uint16_t> a;
        for (int i = 0; i < 40; i++) a.push_back((uint16_t)(rng() & 0x3ff));
        static const uint16_t directed[] = {
            0b0000011111, 0b1101010100, 0b0010101011, 0b0101010100,
            0b1010101011, 0x000, 0x3ff, 0b0101010101, 0b1010101010,
        };
        for (uint16_t w : directed) a.push_back(w);
        for (int i = 0; i < 120; i++) a.push_back((uint16_t)(rng() & 0x3ff));
        char tag[32];
        snprintf(tag, sizeof tag, "offset %d run A", offset);
        check_stream(run_words(offset, a), a, tag);

        // Run B: reset mid-stream, then fresh random words — re-lock
        // must work exactly like the cold lock.
        reset_dut(offset);
        std::vector<uint16_t> b;
        for (int i = 0; i < 200; i++) b.push_back((uint16_t)(rng() & 0x3ff));
        snprintf(tag, sizeof tag, "offset %d run B", offset);
        check_stream(run_words(offset, b), b, tag);

        delete dut;
    }

    printf("video_serializer: %d checks, %d failures\n", checks, errors);
    if (errors == 0)
        printf("  PASS: locked LSB-first stream at all five clock phasings\n");
    return errors ? 1 : 0;
}

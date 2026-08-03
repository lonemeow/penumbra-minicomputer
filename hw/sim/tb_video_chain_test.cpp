// Verilator testbench for the complete text-video output chain
// (hw/rtl/io/video/video_chain_test.sv).
//
// Plays the monitor at the end of the GPDI cable. The chain runs free
// for two frames while the testbench records all four DDR lane
// streams; it then recovers the image the way a DVI receiver does,
// with no knowledge of the chain's internal latencies:
//   1. character-align: try all ten bit phases on the blue lane and
//      keep the one where blanking is full of valid control codes
//   2. TMDS-decode every 10-bit symbol on all three data lanes
//   3. find a frame boundary from the vsync deassert edge carried in
//      the blue lane's control codes (the frame's first active pixel
//      follows exactly V_BACK lines later)
//   4. walk one full 800x525 raster cycle-exactly: every active
//      symbol must decode to the pattern oracle's RGB for (x, y), and
//      every blanking symbol must carry the correct hsync/vsync
//      levels (blue) or idle control (green/red)
// The clock lane is checked as a wire pattern: after its first edge,
// a strict 5-bits-high / 5-bits-low cadence — the pixel clock.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vvideo_chain_test.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

// ── VESA 640x480 @ 60 geometry (the spec oracle, restated) ─────────
static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static constexpr int H_TOTAL = 800, V_TOTAL = 525;
static constexpr int H_SYNC_START = H_ACTIVE + H_FRONT;   // 656
static constexpr int V_SYNC_START = V_ACTIVE + V_FRONT;   // 490

// ── Pattern oracle: the test card as specified in video_pattern_gen ─
static void pattern_oracle(int x, int y, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t R = 0, G = 0, B = 0;
    uint8_t ramp = (uint8_t)~y;
    if (x == 0 || x == H_ACTIVE - 1 || y == 0 || y == V_ACTIVE - 1) {
        R = G = B = 0xFF;                       // white border
    } else if (!(y & 0x100)) {
        switch ((x >> 7) & 7) {                 // 128-px vertical bars
            case 0: R = ramp; break;
            case 1: G = ramp; break;
            case 2: B = ramp; break;
            case 3: R = G = B = ramp; break;
            default: break;                     // black bars
        }
    } else {                                    // 8x8 checkerboard
        bool on = (y & 8) ? ((x & 8) != 0) : ((x & 8) == 0);
        if (on) R = G = B = ramp;
    }
    *r = R; *g = G; *b = B;
}

// ── TMDS receive rules ─────────────────────────────────────────────
static const uint16_t CTRL_WORD[4] = { 0b1101010100, 0b0010101011,
                                       0b0101010100, 0b1010101011 };

// Control-code index for a symbol, or -1 if it is a data symbol.
static int ctrl_index(uint16_t q) {
    for (int i = 0; i < 4; i++)
        if (q == CTRL_WORD[i]) return i;
    return -1;
}

static uint8_t tmds_decode(uint16_t q) {
    uint8_t v = q & 0xff;
    if (q & (1 << 9)) v = (uint8_t)~v;
    uint8_t d = v & 1;
    for (int i = 1; i < 8; i++) {
        int bit = ((v >> i) & 1) ^ ((v >> (i - 1)) & 1);
        if (!((q >> 8) & 1)) bit = !bit;
        d |= (uint8_t)bit << i;
    }
    return d;
}

// ── Stimulus ───────────────────────────────────────────────────────
static Vvideo_chain_test* dut;

// Two frames plus lead-in: enough that one complete frame always
// follows the first vsync edge, wherever lock lands.
static constexpr long N_PCLK = (long)H_TOTAL * V_TOTAL * 2 + 5000;
static constexpr int  OFFSET = 3;   // pixel-edge placement in the serial cycle

static void capture(std::vector<uint8_t> bits[4]) {
    for (long cyc = 0; cyc < N_PCLK * 5; cyc++) {
        int ph = (int)(cyc % 5);
        dut->i_sclk = 0;
        if (ph == (OFFSET + 3) % 5) dut->i_pclk = 0;
        dut->eval();
        dut->i_sclk = 1;
        if (ph == OFFSET) dut->i_pclk = 1;
        dut->eval();
        for (int l = 0; l < 4; l++) {
            bits[l].push_back((uint8_t)((dut->o_d0 >> l) & 1));
            bits[l].push_back((uint8_t)((dut->o_d1 >> l) & 1));
        }
    }
}

// Assemble the 10-bit symbol starting at bit position p (LSB-first).
static uint16_t symbol_at(const std::vector<uint8_t>& bits, size_t p) {
    uint16_t q = 0;
    for (int i = 0; i < 10; i++) q |= (uint16_t)bits[p + i] << i;
    return q;
}

int main() {
    dut = new Vvideo_chain_test;

    // Synchronous reset, held two pixel periods (testbench convention).
    dut->i_rst = 1; dut->i_pclk = 0; dut->i_sclk = 0;
    for (int c = 0; c < 12; c++) {
        int ph = c % 5;
        dut->i_sclk = 0; if (ph == (OFFSET + 3) % 5) dut->i_pclk = 0; dut->eval();
        dut->i_sclk = 1; if (ph == OFFSET) dut->i_pclk = 1; dut->eval();
    }
    dut->i_rst = 0;

    static std::vector<uint8_t> bits[4];
    capture(bits);

    // 1. Character alignment: vote over early symbols on the blue lane.
    const long VOTE_SYMS = 5000;
    int phase = -1; long best = -1;
    for (int p = 0; p < 10; p++) {
        long hits = 0;
        for (long s = 0; s < VOTE_SYMS; s++)
            if (ctrl_index(symbol_at(bits[0], (size_t)(10 * s + p))) >= 0) hits++;
        if (hits > best) { best = hits; phase = p; }
    }
    // Roughly a fifth of early symbols are blanking; anything close to
    // zero means no phase looked like control codes at all.
    CHECK(best > VOTE_SYMS / 10,
          "alignment: best phase %d matched only %ld/%ld control symbols",
          phase, best, VOTE_SYMS);
    printf("  aligned at bit phase %d (%ld/%ld control hits)\n",
           phase, best, VOTE_SYMS);

    // 2. Decode symbol streams on the shared phase.
    long n_syms = (long)(bits[0].size() - phase) / 10;
    std::vector<int8_t>  ctrl[3];    // control index or -1
    std::vector<uint8_t> data[3];    // decoded byte (data symbols)
    for (int l = 0; l < 3; l++) {
        ctrl[l].reserve(n_syms);
        data[l].reserve(n_syms);
        for (long s = 0; s < n_syms; s++) {
            uint16_t q = symbol_at(bits[l], (size_t)(10 * s + phase));
            ctrl[l].push_back((int8_t)ctrl_index(q));
            data[l].push_back(tmds_decode(q));
        }
    }

    // 3. Frame boundary: the first vsync deassert edge in the blue
    // control stream; the next frame's (0,0) starts V_BACK lines on.
    long vs_edge = -1;
    for (long s = 1; s < n_syms; s++) {
        if (ctrl[0][s - 1] >= 0 && ctrl[0][s] >= 0 &&
            !(ctrl[0][s - 1] & 2) && (ctrl[0][s] & 2)) { vs_edge = s; break; }
    }
    CHECK(vs_edge >= 0, "no vsync deassert edge found in %ld symbols", n_syms);

    if (vs_edge >= 0) {
        long frame0 = vs_edge + (long)V_BACK * H_TOTAL;
        CHECK(frame0 + (long)H_TOTAL * V_TOTAL <= n_syms,
              "stream too short for a full frame after the vsync edge");

        // 4. Cycle-exact raster walk over one complete frame.
        long bad_pixels = 0, bad_blank = 0;
        for (int y = 0; y < V_TOTAL; y++) {
            for (int x = 0; x < H_TOTAL; x++) {
                long s = frame0 + (long)y * H_TOTAL + x;
                bool active = (x < H_ACTIVE) && (y < V_ACTIVE);
                int  exp_hs = (x >= H_SYNC_START && x < H_SYNC_START + H_SYNC) ? 0 : 1;
                int  exp_vs = (y >= V_SYNC_START && y < V_SYNC_START + V_SYNC) ? 0 : 1;
                if (active) {
                    uint8_t er, eg, eb;
                    pattern_oracle(x, y, &er, &eg, &eb);
                    bool ok = ctrl[0][s] < 0 && ctrl[1][s] < 0 && ctrl[2][s] < 0
                           && data[0][s] == eb && data[1][s] == eg && data[2][s] == er;
                    if (!ok) bad_pixels++;
                    CHECK(ok, "pixel (%d,%d): got B/G/R %02x/%02x/%02x "
                              "(ctrl %d/%d/%d), expected %02x/%02x/%02x",
                          x, y, data[0][s], data[1][s], data[2][s],
                          ctrl[0][s], ctrl[1][s], ctrl[2][s], eb, eg, er);
                } else {
                    int exp_idx = (exp_vs << 1) | exp_hs;
                    bool ok = ctrl[0][s] == exp_idx
                           && ctrl[1][s] == 0 && ctrl[2][s] == 0;
                    if (!ok) bad_blank++;
                    CHECK(ok, "blank (%d,%d): got ctrl %d/%d/%d, expected %d/0/0",
                          x, y, ctrl[0][s], ctrl[1][s], ctrl[2][s], exp_idx);
                }
            }
        }
        printf("  frame at symbol %ld: %ld bad active, %ld bad blanking of %d\n",
               frame0, bad_pixels, bad_blank, H_TOTAL * V_TOTAL);
    }

    // Clock lane: after the first edge, strict runs of five — the
    // 25 MHz pixel clock as a wire pattern, any phase.
    {
        const std::vector<uint8_t>& ck = bits[3];
        size_t first_edge = 1;
        while (first_edge < ck.size() && ck[first_edge] == ck[first_edge - 1])
            first_edge++;
        long run = 0, bad_runs = 0;
        for (size_t i = first_edge; i + 1 < ck.size(); i++) {
            run++;
            if (ck[i + 1] != ck[i]) {
                if (run != 5) bad_runs++;
                run = 0;
            }
        }
        CHECK(bad_runs == 0, "clock lane: %ld runs deviate from 5 bits", bad_runs);
    }

    printf("video_chain_test: %d checks, %d failures\n", checks, errors);
    if (errors == 0)
        printf("  PASS: full frame recovered bit-exact through the TMDS link\n");

    delete dut;
    return errors ? 1 : 0;
}

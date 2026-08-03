// Verilator testbench for the Penumbra TMDS line encoder
// (hw/rtl/io/video/video_tmds_encoder.sv).
//
// The oracle is a direct transliteration of the DVI 1.0 encode flow
// (Figure 3-5, page 29): serial XOR/XNOR chain with a per-bit mux and a
// running disparity in spec bit-units — deliberately a different
// formulation from the RTL's prefix-parity / halved-units one, so a
// shared misreading cannot hide.
//
// Three independent checks per active pixel, sampled one cycle after
// the input (the RTL registers its output once):
//   - o_tmds matches the oracle word bit-exactly
//   - decoding o_tmds per the DVI receive rules recovers the input byte
//     (contract-level: holds even if the oracle itself were wrong)
//   - the oracle's running disparity stays within the spec's +/-10 bits
// Blanking cycles must produce the control code for {c1,c0} and reset
// the disparity. The module's own track_word / disp_bound assertions
// are armed via --assert and fail the run through Verilator.

#include <cstdio>
#include <cstdint>
#include "Vvideo_tmds_encoder.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

// ── Oracle: DVI 1.0 Figure 3-5, spec formulation ───────────────────

static int ref_cnt;   // running disparity, spec bit units (1s minus 0s)

static int popcount8(uint8_t v) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += (v >> i) & 1;
    return n;
}

static uint16_t ref_encode(uint8_t d, int de, int c1, int c0) {
    static const uint16_t ctrl[4] = { 0b1101010100, 0b0010101011,
                                      0b0101010100, 0b1010101011 };
    if (!de) { ref_cnt = 0; return ctrl[(c1 << 1) | c0]; }

    int n1d = popcount8(d);
    int use_xnor = (n1d > 4) || (n1d == 4 && !(d & 1));
    uint16_t qm = d & 1;
    for (int i = 1; i < 8; i++) {
        int x = ((qm >> (i - 1)) & 1) ^ ((d >> i) & 1);
        qm |= (uint16_t)(use_xnor ? !x : x) << i;
    }
    if (!use_xnor) qm |= 1 << 8;

    int n1 = popcount8(qm & 0xff), n0 = 8 - n1;
    int qm8 = (qm >> 8) & 1;
    uint16_t q;
    if (ref_cnt == 0 || n1 == n0) {
        q = (uint16_t)((!qm8) << 9) | (uint16_t)(qm8 << 8)
          | (qm8 ? (qm & 0xff) : (~qm & 0xff));
        ref_cnt += qm8 ? (n1 - n0) : (n0 - n1);
    } else if ((ref_cnt > 0 && n1 > n0) || (ref_cnt < 0 && n1 < n0)) {
        q = (uint16_t)(1 << 9) | (uint16_t)(qm8 << 8) | (~qm & 0xff);
        ref_cnt += 2 * qm8 + (n0 - n1);
    } else {
        q = (uint16_t)(qm8 << 8) | (qm & 0xff);
        ref_cnt += -2 * (!qm8) + (n1 - n0);
    }
    return q;
}

// DVI receive rules: undo the inversion marked by bit 9, then undo the
// chain marked by bit 8 (XOR: d[i] = v[i]^v[i-1]; XNOR: complemented).
static uint8_t ref_decode(uint16_t q) {
    uint8_t v = q & 0xff;
    if (q & (1 << 9)) v = (uint8_t)~v;
    uint8_t d = v & 1;
    for (int i = 1; i < 8; i++) {
        int b = ((v >> i) & 1) ^ ((v >> (i - 1)) & 1);
        if (!((q >> 8) & 1)) b = !b;
        d |= (uint8_t)b << i;
    }
    return d;
}

// ── Stimulus ───────────────────────────────────────────────────────

static uint32_t rng_state = 1;
static uint32_t rng() {   // xorshift32 — deterministic across platforms
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static Vvideo_tmds_encoder* dut;

static void tick() {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Drive one input cycle, advance the clock, and check the registered
// output against the oracle (and decodability, for active pixels).
static void step(uint8_t d, int de, int c1, int c0) {
    dut->i_data = d;
    dut->i_de   = de;
    dut->i_c1   = c1;
    dut->i_c0   = c0;
    uint16_t expect = ref_encode(d, de, c1, c0);
    tick();
    CHECK(dut->o_tmds == expect,
          "%s d=%02x c=%d%d: got %03x, expected %03x (ref_cnt=%d)",
          de ? "active" : "blank", d, c1, c0,
          dut->o_tmds, expect, ref_cnt);
    if (de) {
        CHECK(ref_decode(dut->o_tmds) == d,
              "decode: word %03x decodes to %02x, input was %02x",
              dut->o_tmds, ref_decode(dut->o_tmds), d);
        CHECK(ref_cnt >= -10 && ref_cnt <= 10,
              "oracle disparity out of spec bound: %d after d=%02x",
              ref_cnt, d);
    }
}

static void blank(int cycles) {
    for (int i = 0; i < cycles; i++)
        step((uint8_t)rng(), 0, (int)(rng() & 1), (int)(rng() & 1));
}

int main() {
    dut = new Vvideo_tmds_encoder;

    // Synchronous reset, held two cycles (testbench convention).
    dut->i_rst = 1; dut->i_clk = 0;
    dut->i_data = 0; dut->i_de = 0; dut->i_c1 = 0; dut->i_c0 = 0;
    dut->eval();
    tick();
    tick();
    dut->i_rst = 0;
    dut->eval();
    CHECK(dut->o_tmds == 0b1101010100,
          "reset word: got %03x, expected the c=00 control code", dut->o_tmds);

    // All four control codes, held for runs like real sync pulses.
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 8; i++)
            step((uint8_t)rng(), 0, (c >> 1) & 1, c & 1);

    // Every byte value from a recentered line: the balanced/centered
    // branch and the first steering decision after it.
    for (int v = 0; v < 256; v++) {
        blank(2);
        step((uint8_t)v, 1, 0, 0);
        step((uint8_t)v, 1, 0, 0);   // same byte again, disparity now nonzero
    }

    // Every byte value inside one long run: disparity arrives in
    // whatever state the previous bytes left it.
    blank(4);
    for (int v = 0; v < 256; v++)
        step((uint8_t)v, 1, 0, 0);

    // Adversarial runs: extreme bytes force maximum steering, and the
    // control bits must be ignored during active video.
    static const uint8_t hard[] = { 0x00, 0xff, 0x01, 0x80, 0x55, 0xaa, 0x10, 0xef };
    for (unsigned h = 0; h < sizeof(hard); h++) {
        blank(3);
        for (int i = 0; i < 64; i++)
            step(hard[h], 1, (int)(rng() & 1), (int)(rng() & 1));
    }

    // Random soak: full 800-cycle "scanlines", 640 active + blanking,
    // random bytes and random in-active control bits.
    for (int line = 0; line < 32; line++) {
        for (int x = 0; x < 640; x++)
            step((uint8_t)rng(), 1, (int)(rng() & 1), (int)(rng() & 1));
        blank(160);
    }

    printf("video_tmds_encoder: %d checks, %d failures\n", checks, errors);
    if (errors == 0)
        printf("  PASS: bit-exact with the DVI 1.0 reference encoder\n");

    delete dut;
    return errors ? 1 : 0;
}

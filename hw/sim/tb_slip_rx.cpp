// Verilator testbench for SLIP RX decoder
//
// Tests RFC 1055 SLIP framing: END delimiters, escape sequences,
// invalid escapes, empty frames, and back-to-back frames.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vslip_rx.h"

static int errors = 0, tests = 0;

static void tick(Vslip_rx* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vslip_rx* d) {
    d->i_rst = 1;
    d->i_valid = 0;
    d->i_data = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

// Feed one byte into the decoder
static void feed(Vslip_rx* d, uint8_t byte) {
    d->i_valid = 1;
    d->i_data = byte;
    tick(d);
    d->i_valid = 0;
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

// Feed a SLIP-encoded byte stream, collect decoded output bytes
// and frame_end pulses.
struct DecodeResult {
    std::vector<uint8_t> bytes;
    int frame_ends;
};

static DecodeResult decode_stream(Vslip_rx* d,
                                  const std::vector<uint8_t>& stream) {
    DecodeResult r = {{}, 0};
    reset(d);
    for (uint8_t b : stream) {
        d->i_valid = 1;
        d->i_data = b;
        d->eval();  // combinational outputs settle before clock
        if (d->o_valid)
            r.bytes.push_back(d->o_data);
        if (d->o_frame_end)
            r.frame_ends++;
        d->i_clk = 0; d->eval();
        d->i_clk = 1; d->eval();
    }
    d->i_valid = 0;
    return r;
}

// ── Test: plain bytes pass through ────────────────────────────
static void test_passthrough(Vslip_rx* d) {
    printf("test_passthrough\n");
    auto r = decode_stream(d, {0x41, 0x42, 0x43});
    CHECK("3 bytes out", r.bytes.size() == 3);
    CHECK("byte 0 = 0x41", r.bytes[0] == 0x41);
    CHECK("byte 1 = 0x42", r.bytes[1] == 0x42);
    CHECK("byte 2 = 0x43", r.bytes[2] == 0x43);
    CHECK("no frame_end", r.frame_ends == 0);
}

// ── Test: END delimiter signals frame end, no data byte ───────
static void test_end_delimiter(Vslip_rx* d) {
    printf("test_end_delimiter\n");
    auto r = decode_stream(d, {0x41, 0xC0});
    CHECK("1 byte out", r.bytes.size() == 1);
    CHECK("byte = 0x41", r.bytes[0] == 0x41);
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: ESC + 0xDC decodes to literal 0xC0 ─────────────────
static void test_esc_end(Vslip_rx* d) {
    printf("test_esc_end\n");
    auto r = decode_stream(d, {0xDB, 0xDC, 0xC0});
    CHECK("1 byte out", r.bytes.size() == 1);
    CHECK("byte = 0xC0", r.bytes[0] == 0xC0);
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: ESC + 0xDD decodes to literal 0xDB ─────────────────
static void test_esc_esc(Vslip_rx* d) {
    printf("test_esc_esc\n");
    auto r = decode_stream(d, {0xDB, 0xDD, 0xC0});
    CHECK("1 byte out", r.bytes.size() == 1);
    CHECK("byte = 0xDB", r.bytes[0] == 0xDB);
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: invalid escape (ESC + random byte) passes through ───
static void test_invalid_escape(Vslip_rx* d) {
    printf("test_invalid_escape\n");
    auto r = decode_stream(d, {0xDB, 0x42, 0xC0});
    CHECK("1 byte out", r.bytes.size() == 1);
    CHECK("byte = 0x42 (passed through)", r.bytes[0] == 0x42);
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: empty frame (just END) ──────────────────────────────
static void test_empty_frame(Vslip_rx* d) {
    printf("test_empty_frame\n");
    auto r = decode_stream(d, {0xC0});
    CHECK("0 bytes out", r.bytes.empty());
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: back-to-back frames ─────────────────────────────────
static void test_back_to_back(Vslip_rx* d) {
    printf("test_back_to_back\n");
    // Frame 1: [0x01, 0x02], END, Frame 2: [0x03], END
    auto r = decode_stream(d, {0x01, 0x02, 0xC0, 0x03, 0xC0});
    CHECK("3 bytes total", r.bytes.size() == 3);
    CHECK("byte 0 = 0x01", r.bytes[0] == 0x01);
    CHECK("byte 1 = 0x02", r.bytes[1] == 0x02);
    CHECK("byte 2 = 0x03", r.bytes[2] == 0x03);
    CHECK("2 frame_ends", r.frame_ends == 2);
}

// ── Test: consecutive escape sequences ────────────────────────
static void test_consecutive_escapes(Vslip_rx* d) {
    printf("test_consecutive_escapes\n");
    // ESC+DC, ESC+DD, ESC+DC → 0xC0, 0xDB, 0xC0
    auto r = decode_stream(d, {0xDB, 0xDC, 0xDB, 0xDD, 0xDB, 0xDC, 0xC0});
    CHECK("3 bytes out", r.bytes.size() == 3);
    CHECK("byte 0 = 0xC0", r.bytes[0] == 0xC0);
    CHECK("byte 1 = 0xDB", r.bytes[1] == 0xDB);
    CHECK("byte 2 = 0xC0", r.bytes[2] == 0xC0);
    CHECK("1 frame_end", r.frame_ends == 1);
}

// ── Test: leading END (common SLIP convention) ────────────────
static void test_leading_end(Vslip_rx* d) {
    printf("test_leading_end\n");
    // Some SLIP implementations send END before frame data to flush noise
    auto r = decode_stream(d, {0xC0, 0x41, 0x42, 0xC0});
    CHECK("2 bytes out", r.bytes.size() == 2);
    CHECK("byte 0 = 0x41", r.bytes[0] == 0x41);
    CHECK("byte 1 = 0x42", r.bytes[1] == 0x42);
    CHECK("2 frame_ends (leading + trailing)", r.frame_ends == 2);
}

// ── Test: no output when i_valid is low ───────────────────────
static void test_idle(Vslip_rx* d) {
    printf("test_idle\n");
    reset(d);
    // Several ticks with i_valid = 0
    d->i_valid = 0;
    d->i_data = 0x41;
    for (int i = 0; i < 10; i++) {
        tick(d);
        CHECK("no valid output when idle", d->o_valid == 0);
        CHECK("no frame_end when idle", d->o_frame_end == 0);
    }
}

// ── Test: mixed data and escapes in a larger frame ────────────
static void test_mixed_frame(Vslip_rx* d) {
    printf("test_mixed_frame\n");
    // 0x01, ESC+DC, 0x02, ESC+DD, 0x03, END
    // Expected: 0x01, 0xC0, 0x02, 0xDB, 0x03
    auto r = decode_stream(d, {0x01, 0xDB, 0xDC, 0x02, 0xDB, 0xDD, 0x03, 0xC0});
    CHECK("5 bytes out", r.bytes.size() == 5);
    CHECK("byte 0 = 0x01", r.bytes[0] == 0x01);
    CHECK("byte 1 = 0xC0", r.bytes[1] == 0xC0);
    CHECK("byte 2 = 0x02", r.bytes[2] == 0x02);
    CHECK("byte 3 = 0xDB", r.bytes[3] == 0xDB);
    CHECK("byte 4 = 0x03", r.bytes[4] == 0x03);
    CHECK("1 frame_end", r.frame_ends == 1);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* d = new Vslip_rx;

    test_passthrough(d);
    test_end_delimiter(d);
    test_esc_end(d);
    test_esc_esc(d);
    test_invalid_escape(d);
    test_empty_frame(d);
    test_back_to_back(d);
    test_consecutive_escapes(d);
    test_leading_end(d);
    test_idle(d);
    test_mixed_frame(d);

    printf("\n%d/%d tests passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}

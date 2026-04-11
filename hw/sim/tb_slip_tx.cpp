// Verilator testbench for SLIP TX encoder
//
// Tests RFC 1055 SLIP encoding: plain bytes, escape sequences for
// 0xC0 and 0xDB, frame END delimiter, back-pressure (o_busy), and
// back-to-back frames.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vslip_tx.h"

static int errors = 0, tests = 0;

static void tick(Vslip_tx* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vslip_tx* d) {
    d->i_rst = 1;
    d->i_valid = 0;
    d->i_data = 0;
    d->i_frame_end = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

// Feed payload bytes and a frame END, collect all encoded output.
// Respects o_busy: doesn't advance to next input byte while busy.
struct EncodeResult {
    std::vector<uint8_t> bytes;
};

static EncodeResult encode_frame(Vslip_tx* d,
                                 const std::vector<uint8_t>& payload) {
    EncodeResult r;
    reset(d);

    for (size_t i = 0; i < payload.size(); ) {
        if (!d->o_busy) {
            d->i_valid = 1;
            d->i_data = payload[i];
            d->i_frame_end = 0;
        } else {
            d->i_valid = 0;
            d->i_frame_end = 0;
        }
        d->eval();
        if (d->o_valid)
            r.bytes.push_back(d->o_data);
        d->i_clk = 0; d->eval();
        d->i_clk = 1; d->eval();

        if (!d->o_busy)
            i++;  // advance only when not busy
    }

    // Drain any pending escape byte
    d->i_valid = 0;
    d->i_frame_end = 0;
    while (d->o_busy) {
        d->eval();
        if (d->o_valid)
            r.bytes.push_back(d->o_data);
        tick(d);
    }

    // Send frame END
    d->i_frame_end = 1;
    d->i_valid = 0;
    d->eval();
    if (d->o_valid)
        r.bytes.push_back(d->o_data);
    tick(d);
    d->i_frame_end = 0;

    return r;
}

// ── Test: plain bytes pass through unchanged ──────────────────
static void test_passthrough(Vslip_tx* d) {
    printf("test_passthrough\n");
    auto r = encode_frame(d, {0x41, 0x42, 0x43});
    CHECK("4 bytes out (3 + END)", r.bytes.size() == 4);
    CHECK("byte 0 = 0x41", r.bytes[0] == 0x41);
    CHECK("byte 1 = 0x42", r.bytes[1] == 0x42);
    CHECK("byte 2 = 0x43", r.bytes[2] == 0x43);
    CHECK("byte 3 = END (0xC0)", r.bytes[3] == 0xC0);
}

// ── Test: 0xC0 in payload gets escaped to ESC + ESC_END ───────
static void test_escape_end(Vslip_tx* d) {
    printf("test_escape_end\n");
    auto r = encode_frame(d, {0xC0});
    CHECK("3 bytes out (ESC + ESC_END + END)", r.bytes.size() == 3);
    CHECK("byte 0 = ESC (0xDB)", r.bytes[0] == 0xDB);
    CHECK("byte 1 = ESC_END (0xDC)", r.bytes[1] == 0xDC);
    CHECK("byte 2 = END (0xC0)", r.bytes[2] == 0xC0);
}

// ── Test: 0xDB in payload gets escaped to ESC + ESC_ESC ───────
static void test_escape_esc(Vslip_tx* d) {
    printf("test_escape_esc\n");
    auto r = encode_frame(d, {0xDB});
    CHECK("3 bytes out (ESC + ESC_ESC + END)", r.bytes.size() == 3);
    CHECK("byte 0 = ESC (0xDB)", r.bytes[0] == 0xDB);
    CHECK("byte 1 = ESC_ESC (0xDD)", r.bytes[1] == 0xDD);
    CHECK("byte 2 = END (0xC0)", r.bytes[2] == 0xC0);
}

// ── Test: empty frame (just END) ──────────────────────────────
static void test_empty_frame(Vslip_tx* d) {
    printf("test_empty_frame\n");
    auto r = encode_frame(d, {});
    CHECK("1 byte out (END)", r.bytes.size() == 1);
    CHECK("byte 0 = END (0xC0)", r.bytes[0] == 0xC0);
}

// ── Test: o_busy asserted during escape sequences ─────────────
static void test_busy_signal(Vslip_tx* d) {
    printf("test_busy_signal\n");
    reset(d);

    // Feed 0xC0 — should trigger escape
    d->i_valid = 1;
    d->i_data = 0xC0;
    d->i_frame_end = 0;
    d->eval();
    CHECK("first byte = ESC (0xDB)", d->o_data == 0xDB);
    CHECK("first byte valid", d->o_valid == 1);
    tick(d);

    // After clock: should be busy (sending second byte of escape)
    d->i_valid = 0;
    CHECK("busy after ESC", d->o_busy == 1);
    d->eval();
    CHECK("second byte = ESC_END (0xDC)", d->o_data == 0xDC);
    CHECK("second byte valid", d->o_valid == 1);
    tick(d);

    // Should return to idle
    CHECK("not busy after escape complete", d->o_busy == 0);
}

// ── Test: consecutive special bytes ───────────────────────────
static void test_consecutive_specials(Vslip_tx* d) {
    printf("test_consecutive_specials\n");
    // 0xC0, 0xDB → ESC DC ESC DD + END
    auto r = encode_frame(d, {0xC0, 0xDB});
    CHECK("5 bytes out", r.bytes.size() == 5);
    CHECK("byte 0 = ESC", r.bytes[0] == 0xDB);
    CHECK("byte 1 = ESC_END", r.bytes[1] == 0xDC);
    CHECK("byte 2 = ESC", r.bytes[2] == 0xDB);
    CHECK("byte 3 = ESC_ESC", r.bytes[3] == 0xDD);
    CHECK("byte 4 = END", r.bytes[4] == 0xC0);
}

// ── Test: mixed data and special bytes ────────────────────────
static void test_mixed(Vslip_tx* d) {
    printf("test_mixed\n");
    // 0x01, 0xC0, 0x02, 0xDB, 0x03
    // → 0x01, ESC, DC, 0x02, ESC, DD, 0x03, END
    auto r = encode_frame(d, {0x01, 0xC0, 0x02, 0xDB, 0x03});
    CHECK("8 bytes out", r.bytes.size() == 8);
    CHECK("byte 0 = 0x01", r.bytes[0] == 0x01);
    CHECK("byte 1 = ESC", r.bytes[1] == 0xDB);
    CHECK("byte 2 = ESC_END", r.bytes[2] == 0xDC);
    CHECK("byte 3 = 0x02", r.bytes[3] == 0x02);
    CHECK("byte 4 = ESC", r.bytes[4] == 0xDB);
    CHECK("byte 5 = ESC_ESC", r.bytes[5] == 0xDD);
    CHECK("byte 6 = 0x03", r.bytes[6] == 0x03);
    CHECK("byte 7 = END", r.bytes[7] == 0xC0);
}

// ── Test: no output when idle ─────────────────────────────────
static void test_idle(Vslip_tx* d) {
    printf("test_idle\n");
    reset(d);
    d->i_valid = 0;
    d->i_frame_end = 0;
    for (int i = 0; i < 10; i++) {
        tick(d);
        CHECK("no valid output when idle", d->o_valid == 0);
        CHECK("not busy when idle", d->o_busy == 0);
    }
}

// ── Test: frame END without any data ──────────────────────────
static void test_end_only(Vslip_tx* d) {
    printf("test_end_only\n");
    reset(d);
    d->i_valid = 0;
    d->i_frame_end = 1;
    d->eval();
    CHECK("END byte valid", d->o_valid == 1);
    CHECK("END byte = 0xC0", d->o_data == 0xC0);
    tick(d);
    d->i_frame_end = 0;
}

// ── Test: all-special-byte frame ──────────────────────────────
static void test_all_special(Vslip_tx* d) {
    printf("test_all_special\n");
    // 0xC0, 0xC0, 0xDB, 0xDB
    // → ESC DC ESC DC ESC DD ESC DD END = 9 bytes
    auto r = encode_frame(d, {0xC0, 0xC0, 0xDB, 0xDB});
    CHECK("9 bytes out", r.bytes.size() == 9);
    // Each special byte becomes 2 bytes
    CHECK("byte 0 = ESC", r.bytes[0] == 0xDB);
    CHECK("byte 1 = ESC_END", r.bytes[1] == 0xDC);
    CHECK("byte 2 = ESC", r.bytes[2] == 0xDB);
    CHECK("byte 3 = ESC_END", r.bytes[3] == 0xDC);
    CHECK("byte 4 = ESC", r.bytes[4] == 0xDB);
    CHECK("byte 5 = ESC_ESC", r.bytes[5] == 0xDD);
    CHECK("byte 6 = ESC", r.bytes[6] == 0xDB);
    CHECK("byte 7 = ESC_ESC", r.bytes[7] == 0xDD);
    CHECK("byte 8 = END", r.bytes[8] == 0xC0);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto* d = new Vslip_tx;

    test_passthrough(d);
    test_escape_end(d);
    test_escape_esc(d);
    test_empty_frame(d);
    test_busy_signal(d);
    test_consecutive_specials(d);
    test_mixed(d);
    test_idle(d);
    test_end_only(d);
    test_all_special(d);

    printf("\n%d/%d tests passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}

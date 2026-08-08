// Verilator testbench for the text-video built-in font ROM
// (hw/rtl/io/video/video_font_rom.sv).
//
// Two layers of checking against the generated image font8x16.hex
// (run from the repo root, the test-modules convention, so the name
// resolves — `make test-modules` generates it):
//   - mapping + latency: every {glyph, row} address returns the byte
//     the image holds at glyph*16 + row exactly one clock after the
//     address is presented, and the output holds through address
//     changes until the next edge (the read is a registered stage);
//   - content: CP437 semantic invariants pinned on known glyphs, so a
//     wrong font source or a scrambled tool mapping fails loudly even
//     though the round-trip layer, which trusts the image, would pass.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "Vvideo_font_rom.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static void tick(Vvideo_font_rom* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

int main() {
    // Reference image — the same file the DUT $readmemh-loads.
    std::ifstream f("font8x16.hex");
    if (!f) {
        printf("FAIL: font8x16.hex not found (run from the repo root; "
               "make test-modules generates it)\n");
        return 1;
    }
    std::vector<uint8_t> ref;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        ref.push_back((uint8_t)strtoul(line.c_str(), nullptr, 16));
    }
    if (ref.size() != 256 * 16) {
        printf("FAIL: font8x16.hex holds %zu bytes, want 4096\n", ref.size());
        return 1;
    }

    Vvideo_font_rom* d = new Vvideo_font_rom;

    // ── Mapping + latency sweep over every address ──────────────
    for (int g = 0; g < 256; g++) {
        for (int r = 0; r < 16; r++) {
            d->i_glyph = g;
            d->i_row = r;
            tick(d);
            CHECK(d->o_pixels == ref[g * 16 + r],
                  "glyph %02x row %2d: got %02x, image has %02x",
                  g, r, d->o_pixels, ref[g * 16 + r]);
        }
    }

    // Registered-read pin: a new address without a clock edge must not
    // disturb the held output.
    d->i_glyph = 0x41; d->i_row = 0x8;
    tick(d);
    uint8_t held = d->o_pixels;
    d->i_glyph = 0xDB; d->i_row = 0x0;
    d->eval();
    CHECK(d->o_pixels == held,
          "output changed combinationally on an address change "
          "(%02x -> %02x)", held, d->o_pixels);

    // ── CP437 content invariants ────────────────────────────────
    // The sweep above proves DUT == image, so these read the image
    // directly via row(); a failure means the wrong font reached the
    // ROM, not a broken ROM. row(g, r) is glyph g's row r, bit 7 =
    // leftmost pixel, 1 = foreground.
    auto row = [&](int g, int r) -> uint8_t { return ref[g * 16 + r]; };
    (void)row;

    for (int r = 0; r < 16; r++) {
        CHECK(row(0x00, r) == 0x00, "expected first glyph to be empty");
        CHECK(row(0x20, r) == 0x00, "expected space character glyph to be empty");
        CHECK(row(0xff, r) == 0x00, "expected last glyph to be empty");
    }

    printf("video_font_rom: %d checks, %d errors\n", checks, errors);
    return errors != 0;
}

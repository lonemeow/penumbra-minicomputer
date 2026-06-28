// Verilator testbench for the Penumbra USB NRZI decoder
//
// Round-trip check: a C++ reference encodes random data bits into line levels
// (0 toggles, 1 holds), the RTL decoder recovers them, and the recovered bits
// must equal the originals. Because the decoder seeds its reference level to
// the encoder's reset level, the very first bit decodes correctly too.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_nrzi_decode.h"

static void settle(Vusb_nrzi_decode* dut) { dut->i_clk = 0; dut->eval(); }
static void edge(Vusb_nrzi_decode* dut)   { dut->i_clk = 1; dut->eval(); }

// Reference encoder, to produce the line levels the decoder consumes.
static std::vector<int> ref_encode(const std::vector<int>& bits) {
    std::vector<int> line;
    int level = 0;
    for (int b : bits) {
        level = b ? level : !level;
        line.push_back(level);
    }
    return line;
}

static std::vector<int> run_decode(Vusb_nrzi_decode* dut, const std::vector<int>& line) {
    dut->i_rst = 1; dut->i_en = 0; dut->i_line = 0;
    settle(dut); edge(dut);
    settle(dut); edge(dut);
    dut->i_rst = 0;

    std::vector<int> bits;
    dut->i_en = 1;
    for (int level : line) {
        dut->i_line = level;
        settle(dut);                   // o_data_bit is combinational for this level
        bits.push_back(dut->o_data_bit);
        edge(dut);                     // latch the level for the next compare
    }
    dut->i_en = 0;
    return bits;
}

static std::vector<std::vector<int>> build_vectors() {
    std::vector<std::vector<int>> v = {
        {},
        {1, 1, 1, 1},
        {0, 0, 0, 0},
        {1, 0, 1, 0, 1, 0},
        {0, 1, 1, 0, 0, 1},
    };
    uint32_t lcg = 0x89abcdefu;
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 27);    // length 0..31
        std::vector<int> bits;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            bits.push_back((lcg >> 31) & 1);
        }
        v.push_back(bits);
    }
    return v;
}

int main() {
    Vusb_nrzi_decode* dut = new Vusb_nrzi_decode;
    auto vectors = build_vectors();
    int pass = 0, fail = 0;
    for (const auto& bits : vectors) {
        std::vector<int> recovered = run_decode(dut, ref_encode(bits));
        if (recovered == bits) pass++;
        else { printf("MISMATCH on a %zu-bit stream\n", bits.size()); fail++; }
    }
    printf("usb_nrzi_decode: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

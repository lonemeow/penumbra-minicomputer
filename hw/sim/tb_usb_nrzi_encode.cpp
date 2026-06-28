// Verilator testbench for the Penumbra USB NRZI encoder
//
// Checks the emitted line levels against a C++ reference (0 toggles, 1 holds)
// over directed and deterministic-random bit streams.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_nrzi_encode.h"

static void settle(Vusb_nrzi_encode* dut) { dut->i_clk = 0; dut->eval(); }
static void edge(Vusb_nrzi_encode* dut)   { dut->i_clk = 1; dut->eval(); }

// Reference: a 0 flips the level, a 1 holds it; level starts at 0.
static std::vector<int> ref_encode(const std::vector<int>& bits) {
    std::vector<int> line;
    int level = 0;
    for (int b : bits) {
        level = b ? level : !level;
        line.push_back(level);
    }
    return line;
}

static std::vector<int> run_encode(Vusb_nrzi_encode* dut, const std::vector<int>& bits) {
    dut->i_rst = 1; dut->i_en = 0; dut->i_data_bit = 0;
    settle(dut); edge(dut);
    settle(dut); edge(dut);
    dut->i_rst = 0;

    std::vector<int> line;
    dut->i_en = 1;
    for (int b : bits) {
        dut->i_data_bit = b;
        settle(dut);                   // o_line is combinational for this bit
        line.push_back(dut->o_line);
        edge(dut);                     // latch the level
    }
    dut->i_en = 0;
    return line;
}

static std::vector<std::vector<int>> build_vectors() {
    std::vector<std::vector<int>> v = {
        {},
        {1, 1, 1, 1},                  // all 1s — line never moves
        {0, 0, 0, 0},                  // all 0s — line toggles every bit
        {1, 0, 1, 0, 1, 0},
        {0, 1, 1, 0, 0, 1},
    };
    uint32_t lcg = 0x1234567u;
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
    Vusb_nrzi_encode* dut = new Vusb_nrzi_encode;
    auto vectors = build_vectors();
    int pass = 0, fail = 0;
    for (const auto& bits : vectors) {
        if (run_encode(dut, bits) == ref_encode(bits)) pass++;
        else { printf("MISMATCH on a %zu-bit stream\n", bits.size()); fail++; }
    }
    printf("usb_nrzi_encode: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

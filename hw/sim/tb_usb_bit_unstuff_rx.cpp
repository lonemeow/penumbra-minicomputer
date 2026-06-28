// Verilator testbench for the Penumbra USB receive bit-unstuffer
//
// Two checks:
//   * round-trip — stuff a random data stream (reference), unstuff it in RTL,
//     and require the original data back with no error reported;
//   * error detection — feed illegal streams (seven+ consecutive 1s) and
//     require o_error to fire.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_bit_unstuff_rx.h"

static void settle(Vusb_bit_unstuff_rx* dut) { dut->i_clk = 0; dut->eval(); }
static void edge(Vusb_bit_unstuff_rx* dut)   { dut->i_clk = 1; dut->eval(); }

// Reference stuffer (same rule as usb_bit_stuff_tx): insert a 0 after every six
// consecutive 1s. Used to build legal input streams for the round-trip check.
static std::vector<int> ref_stuff(const std::vector<int>& data) {
    std::vector<int> out;
    int ones = 0;
    for (int b : data) {
        out.push_back(b);
        if (b) { if (++ones == 6) { out.push_back(0); ones = 0; } }
        else ones = 0;
    }
    return out;
}

struct Result { std::vector<int> data; bool error; };

// Feed a line-bit stream through the DUT, collecting the data bits it marks
// valid and whether it ever flagged a stuff error.
static Result run_unstuff(Vusb_bit_unstuff_rx* dut, const std::vector<int>& line) {
    dut->i_rst = 1; dut->i_en = 0; dut->i_line_bit = 0;
    settle(dut); edge(dut);
    settle(dut); edge(dut);
    dut->i_rst = 0;

    Result r;
    r.error = false;
    dut->i_en = 1;
    for (int b : line) {
        dut->i_line_bit = b;
        settle(dut);
        if (dut->o_error) r.error = true;
        if (dut->o_valid) r.data.push_back(dut->o_data_bit);
        edge(dut);
    }
    dut->i_en = 0;
    return r;
}

static std::vector<std::vector<int>> roundtrip_data() {
    std::vector<std::vector<int>> v = {
        {},
        {0, 0, 0},
        std::vector<int>(6, 1),                   // exactly six 1s -> one stuff
        std::vector<int>(12, 1),                  // two stuffs
        {1, 0, 1, 1, 1, 1, 1, 1, 0, 1},
    };
    uint32_t lcg = 0x0c0ffee0u;
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 26);               // length 0..63
        std::vector<int> bits;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            bits.push_back(((lcg >> 30) & 3) != 0);  // bias toward 1s
        }
        v.push_back(bits);
    }
    return v;
}

int main() {
    Vusb_bit_unstuff_rx* dut = new Vusb_bit_unstuff_rx;
    int pass = 0, fail = 0;

    // Round-trip: stuff (reference) then unstuff (RTL) must recover the data
    // with no error reported.
    for (const auto& data : roundtrip_data()) {
        Result r = run_unstuff(dut, ref_stuff(data));
        if (r.data == data && !r.error) {
            pass++;
        } else {
            printf("ROUND-TRIP FAIL on %zu data bits (error=%d)\n",
                   data.size(), r.error);
            fail++;
        }
    }

    // A legal stuffed run of six 1s + the inserted 0 must NOT error.
    {
        std::vector<int> legal = {1, 1, 1, 1, 1, 1, 0};
        Result r = run_unstuff(dut, legal);
        if (!r.error) pass++;
        else { printf("FALSE ERROR on a legal stuffed stream\n"); fail++; }
    }

    // Illegal streams: a seventh consecutive 1 where the stuff 0 was due.
    std::vector<std::vector<int>> illegal = {
        {1, 1, 1, 1, 1, 1, 1},
        {0, 1, 1, 1, 1, 1, 1, 1},
        std::vector<int>(9, 1),
    };
    for (const auto& line : illegal) {
        Result r = run_unstuff(dut, line);
        if (r.error) pass++;
        else { printf("MISSED stuff error on a %zu-bit stream\n", line.size()); fail++; }
    }

    printf("usb_bit_unstuff_rx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

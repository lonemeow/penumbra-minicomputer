// Verilator testbench for the Penumbra USB transmit bit-stuffer
//
// Drives an unstuffed bit stream through the DUT and checks the emitted line
// stream against a C++ reference that inserts a 0 after every six consecutive
// 1s. The driver honors the consume handshake: a stuffed cycle (o_stuff) holds
// the input bit rather than advancing it.
//
// Each stream is one packet: i_init is pulsed first, seeding the run count
// with SYNC's terminating 1 (the USB stuff count includes it), so a stuff 0
// is due after five payload 1s at packet start, and after six anywhere else.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_bit_stuff_tx.h"

// Combinational outputs settle with the clock low; the run counter advances on
// the rising edge.
static void settle(Vusb_bit_stuff_tx* dut) { dut->i_clk = 0; dut->eval(); }
static void edge(Vusb_bit_stuff_tx* dut)   { dut->i_clk = 1; dut->eval(); }

// Reference — the "golden" stuffer: copy each data bit, and after the sixth
// consecutive 1 emit an extra 0 and restart the run. The count is seeded at 1
// by SYNC's terminating 1.
static std::vector<int> ref_stuff(const std::vector<int>& data) {
    std::vector<int> out;
    int ones = 1;   // SYNC's ending 1 is the first bit of the run
    for (int b : data) {
        out.push_back(b);
        if (b) {
            if (++ones == 6) { out.push_back(0); ones = 0; }
        } else {
            ones = 0;
        }
    }
    return out;
}

// Drive `data` through the DUT one bit at a time and collect the line bits.
// A stuffed cycle does not consume the presented bit, so idx only advances
// when o_stuff is low.
static std::vector<int> run_stuff(Vusb_bit_stuff_tx* dut, const std::vector<int>& data) {
    dut->i_rst = 1; dut->i_init = 0; dut->i_en = 0; dut->i_data_bit = 0;
    settle(dut); edge(dut);
    settle(dut); edge(dut);
    dut->i_rst = 0;

    dut->i_init = 1;
    settle(dut); edge(dut);
    dut->i_init = 0;

    std::vector<int> got;
    size_t idx = 0;
    dut->i_en = 1;
    while (idx < data.size()) {
        dut->i_data_bit = data[idx];
        settle(dut);                       // outputs for the current run count
        got.push_back(dut->o_line_bit);
        bool stuffed = dut->o_stuff;
        edge(dut);                         // latch the next run count
        if (!stuffed) idx++;
    }
    // If the data ended on the sixth consecutive 1, one stuff bit is still due.
    settle(dut);
    if (dut->o_stuff) { got.push_back(dut->o_line_bit); edge(dut); }
    dut->i_en = 0;
    return got;
}

static std::vector<std::vector<int>> build_vectors() {
    std::vector<std::vector<int>> v = {
        {},
        {0, 0, 0},
        {1, 1, 1, 1},                             // four 1s — seeded run not full yet
        {1, 1, 1, 1, 1},                          // five 1s at start — trailing stuff
        {1, 1, 1, 1, 1, 1},                       // six 1s — stuff then a 1
        {1, 1, 1, 1, 1, 0},                       // stuff lands before the 0
        std::vector<int>(11, 1),                  // two stuffs (5 then 6)
        {0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1},
    };

    uint32_t lcg = 0x1234567u;
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 26);               // length 0..63
        std::vector<int> bits;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            // Bias toward 1s so long runs (and stuffing) actually occur.
            bits.push_back(((lcg >> 30) & 3) != 0);
        }
        v.push_back(bits);
    }
    return v;
}

int main() {
    Vusb_bit_stuff_tx* dut = new Vusb_bit_stuff_tx;
    std::vector<std::vector<int>> vectors = build_vectors();
    int pass = 0, fail = 0;

    for (const auto& data : vectors) {
        std::vector<int> got = run_stuff(dut, data);
        std::vector<int> ref = ref_stuff(data);
        if (got == ref) {
            pass++;
        } else {
            printf("MISMATCH (in %zu bits): got %zu line bits, ref %zu\n",
                   data.size(), got.size(), ref.size());
            fail++;
        }
    }

    printf("usb_bit_stuff_tx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

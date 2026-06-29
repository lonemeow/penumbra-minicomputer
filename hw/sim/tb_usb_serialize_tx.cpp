// Verilator testbench for the Penumbra USB transmit serializer
//
// Drives bytes in through the byte handshake and collects the bit stream the
// serializer hands to the bit-stuffer, then requires it equals each byte's
// bits LSB first. Run under several pacings to exercise the two seams:
//   * bit-time always ticking, no back-pressure  -- the simple line;
//   * i_en gated to a slow tick                   -- bit-time pacing;
//   * i_hold injected pseudo-randomly             -- the stuffer's back-pressure
//     stealing bit-times, which must hold the current bit, never drop it.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_serialize_tx.h"

static void settle(Vusb_serialize_tx* dut) { dut->i_clk = 0; dut->eval(); }
static void edge(Vusb_serialize_tx* dut)   { dut->i_clk = 1; dut->eval(); }

// Reference: each byte becomes eight bits, LSB first (USB transmission order).
static std::vector<int> ref_bits(const std::vector<uint8_t>& bytes) {
    std::vector<int> out;
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; i++)
            out.push_back((b >> i) & 1);
    return out;
}

// Feed bytes through the serializer and return the data bits it presents on the
// cycles a bit is actually consumed (o_active && i_en && !i_hold). en_period is
// the i_en duty: 1 ticks every cycle, N ticks one cycle in N. hold_seed != 0
// turns on pseudo-random i_hold to model stuff back-pressure.
static std::vector<int> serialize(Vusb_serialize_tx* dut,
                                  const std::vector<uint8_t>& bytes,
                                  int en_period, uint32_t hold_seed) {
    dut->i_rst = 1; dut->i_en = 0; dut->i_hold = 0;
    dut->i_byte = 0; dut->i_byte_valid = 0;
    settle(dut); edge(dut);
    settle(dut); edge(dut);
    dut->i_rst = 0;

    std::vector<int> got;
    size_t in_idx = 0;
    uint32_t lcg = hold_seed;
    const size_t want = ref_bits(bytes).size();

    for (long c = 0; got.size() < want && c < 1000000; c++) {
        bool offer = (in_idx < bytes.size());
        dut->i_byte       = offer ? bytes[in_idx] : 0;
        dut->i_byte_valid = offer;
        dut->i_en         = (en_period <= 1) ? 1 : ((c % en_period) == 0);
        if (hold_seed) { lcg = lcg * 1664525u + 1013904223u; dut->i_hold = (lcg >> 31) & 1; }
        else dut->i_hold = 0;

        settle(dut);
        bool consumed = dut->i_en && !dut->i_hold;
        if (dut->o_active && consumed) got.push_back(dut->o_data_bit);
        if (dut->o_byte_ready && offer) in_idx++;   // byte latched at this edge
        edge(dut);
    }
    dut->i_en = 0; dut->i_hold = 0; dut->i_byte_valid = 0;
    return got;
}

static std::vector<std::vector<uint8_t>> byte_streams() {
    std::vector<std::vector<uint8_t>> v = {
        {0xB4},                            // directed: bits 0,0,1,0,1,1,0,1
        {0x00},
        {0xFF},
        {0x80, 0x01},                      // SYNC-shaped + a one
        {0xD2, 0xC3, 0x00, 0xFF, 0xA5},
    };
    uint32_t lcg = 0x5eed1234u;
    for (int n = 0; n < 64; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = 1 + (int)((lcg >> 28) & 7);   // 1..8 bytes
        std::vector<uint8_t> bytes;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            bytes.push_back((uint8_t)(lcg >> 24));
        }
        v.push_back(bytes);
    }
    return v;
}

int main() {
    Vusb_serialize_tx* dut = new Vusb_serialize_tx;
    int pass = 0, fail = 0;

    struct Pacing { const char* name; int en_period; uint32_t hold_seed; };
    const Pacing pacings[] = {
        {"continuous",   1, 0},
        {"gated en=3",   3, 0},
        {"gated en=2",   2, 0},
        {"held",         1, 0xdeadbeefu},
        {"gated + held", 3, 0x0badf00du},
    };

    for (const auto& bytes : byte_streams()) {
        std::vector<int> expected = ref_bits(bytes);
        for (const auto& p : pacings) {
            std::vector<int> got = serialize(dut, bytes, p.en_period, p.hold_seed);
            if (got == expected) {
                pass++;
            } else {
                printf("FAIL [%s] on %zu bytes: got %zu bits, want %zu\n",
                       p.name, bytes.size(), got.size(), expected.size());
                fail++;
            }
        }
    }

    printf("usb_serialize_tx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

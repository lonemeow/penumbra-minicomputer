// Verilator testbench for the Penumbra USB data CRC16 generator
//
// Checks the bare LFSR remainder (what usb_crc16.sv outputs) against a C++
// reference model over directed and deterministic-random byte sequences. As
// with CRC5, the on-wire framing (1's-complement + bit-reverse) is the MAC's
// job and out of scope here — see sw/tools/usb_crc.py for that layer.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_crc16.h"

// One clock edge: the registered remainder advances on the rising edge.
static void tick(Vusb_crc16* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

// Seed the remainder via i_init, feed each payload byte, and return the
// settled remainder. Mirrors how the MAC drives the module one byte/cycle.
static uint16_t run_bytes(Vusb_crc16* dut, const std::vector<uint8_t>& data) {
    dut->i_init = 1;
    dut->i_valid = 0;
    dut->i_data = 0;
    tick(dut);
    dut->i_init = 0;
    for (uint8_t b : data) {
        dut->i_valid = 1;
        dut->i_data = b;
        tick(dut);
    }
    dut->i_valid = 0;
    return dut->o_crc & 0xffff;
}

// C++ reference — the "golden" left-shift LFSR for G(x) = x^16+x^15+x^2+1,
// folding each byte LSB-first, step-for-step identical to usb_crc16.sv (and to
// crc16_remainder in usb_crc.py).
static uint16_t ref_crc16_remainder(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xffff;
    for (uint8_t byte : data) {
        for (int i = 0; i < 8; i++) {
            int feedback = ((crc >> 15) & 1) ^ ((byte >> i) & 1);
            crc = (uint16_t)(crc << 1);
            if (feedback)
                crc ^= 0x8005;
        }
    }
    return crc;
}

// The test set: directed edge cases (including a zero-length packet and a
// full-speed-max 64-byte payload) plus a deterministic pseudo-random spread of
// varying lengths (fixed LCG seed, so runs are reproducible).
static std::vector<std::vector<uint8_t>> build_vectors() {
    std::vector<std::vector<uint8_t>> v;
    v.push_back({});                                 // zero-length packet -> seed
    v.push_back({0x00});
    v.push_back({0xff});
    v.push_back({0x01});                             // feedback at the first bit
    v.push_back({0x80});                             // feedback at the last bit
    v.push_back({0x00, 0x00});
    v.push_back({0xde, 0xad, 0xbe, 0xef});

    std::vector<uint8_t> big;                        // 64 B = full-speed max
    for (int i = 0; i < 64; i++) big.push_back((uint8_t)i);
    v.push_back(big);

    uint32_t lcg = 0x1234567u;
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 28);                  // length 0..15
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
    Vusb_crc16* dut = new Vusb_crc16;

    // Power-on reset (held for two edges, matching the testbench convention).
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_init = 0;
    dut->i_valid = 0;
    dut->i_data = 0;
    tick(dut);
    tick(dut);
    dut->i_rst = 0;

    std::vector<std::vector<uint8_t>> vectors = build_vectors();
    int pass = 0, fail = 0;

    for (const auto& data : vectors) {
        uint16_t res = run_bytes(dut, data);
        uint16_t ref = ref_crc16_remainder(data);
        if (res == ref) {
            pass++;
        } else {
            printf("MISMATCH (len %zu): ref = %04x res = %04x\n",
                   data.size(), ref, res);
            fail++;
        }
    }

    printf("usb_crc16: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

// Verilator testbench for the Penumbra USB token CRC5 generator
//
// Checks the bare LFSR remainder (what usb_crc5.sv outputs) against a C++
// reference model over directed and deterministic-random bit sequences. The
// on-wire framing (1's-complement + bit-reverse) is the MAC's job, not this
// module's, so it is intentionally out of scope here — see sw/tools/usb_crc.py
// for that layer and the cross-check vectors.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_crc5.h"

// One clock edge: the registered remainder advances on the rising edge.
static void tick(Vusb_crc5* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

// Seed the remainder via i_init, feed each bit (bits[0] first), and return the
// settled remainder. Mirrors how the MAC will drive the module one bit/cycle.
static uint8_t run_bits(Vusb_crc5* dut, const std::vector<int>& bits) {
    dut->i_init = 1;
    dut->i_valid = 0;
    dut->i_bit = 0;
    tick(dut);
    dut->i_init = 0;
    for (int b : bits) {
        dut->i_valid = 1;
        dut->i_bit = b & 1;
        tick(dut);
    }
    dut->i_valid = 0;
    return dut->o_crc & 0x1f;
}

// C++ reference — the "golden" left-shift LFSR for G(x) = x^5 + x^2 + 1,
// step-for-step identical to usb_crc5.sv (and to crc5_remainder in usb_crc.py).
static uint8_t ref_crc5_remainder(const std::vector<int>& bits) {
    uint8_t crc = 0x1f;
    for (int b : bits) {
        int feedback = ((crc >> 4) & 1) ^ (b & 1);
        crc = (uint8_t)((crc << 1) & 0x1f);
        if (feedback)
            crc ^= 0x05;
    }
    return crc;
}

// Expand an 11-bit USB token field (ADDR[7] then ENDP[4], LSB first) to bits.
static std::vector<int> token_bits(int addr, int endp) {
    std::vector<int> bits;
    for (int i = 0; i < 7; i++) bits.push_back((addr >> i) & 1);
    for (int i = 0; i < 4; i++) bits.push_back((endp >> i) & 1);
    return bits;
}

// The test set: directed edge cases plus a deterministic pseudo-random spread
// of varying lengths (fixed LCG seed, so runs are reproducible).
static std::vector<std::vector<int>> build_vectors() {
    std::vector<std::vector<int>> v;
    v.push_back(std::vector<int>(11, 0));            // seed propagation
    v.push_back({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // feedback at the first bit
    v.push_back({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});  // feedback at the last bit
    v.push_back(token_bits(0x00, 0x0));
    v.push_back(token_bits(0x15, 0xe));
    v.push_back(token_bits(0x3a, 0xa));
    v.push_back(token_bits(0x7f, 0xf));

    uint32_t lcg = 0x1234567u;
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = 1 + (int)(lcg >> 28);              // length 1..16
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
    Vusb_crc5* dut = new Vusb_crc5;

    // Power-on reset (held for two edges, matching the testbench convention).
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_init = 0;
    dut->i_valid = 0;
    dut->i_bit = 0;
    tick(dut);
    tick(dut);
    dut->i_rst = 0;

    std::vector<std::vector<int>> vectors = build_vectors();
    int pass = 0, fail = 0;

    for (const auto &bits : vectors) {
        uint8_t res = run_bits(dut, bits);
        uint8_t ref = ref_crc5_remainder(bits);

        if (res == ref) {
            pass++;
        } else {
            printf("MISMATCH: ref = %02x res = %02x\n", ref, res);
            fail++;
        }
    }

    printf("usb_crc5: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

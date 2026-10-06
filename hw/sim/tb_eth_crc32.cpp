// Verilator testbench for the Penumbra Ethernet frame check sequence
// generator
//
// Checks the bare remainder (what eth_crc32.sv outputs) three ways: against
// a reference that divides by the generator in the other bit order, against
// the check value published for this CRC, and by running it on through the
// check sequence a transmitter would append, which has to leave the residue
// a receiver compares with. Sending the complement and comparing the
// residue are the users' jobs and out of scope here.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Veth_crc32.h"

// What the remainder is after a frame and its own check sequence.
static const uint32_t RESIDUE = 0xDEBB20E3u;

// One clock edge: the registered remainder advances on the rising edge.
static void tick(Veth_crc32* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

// Seed the remainder, as a user does before a frame.
static void seed(Veth_crc32* dut) {
    dut->i_init = 1;
    dut->i_valid = 0;
    dut->i_data = 0;
    tick(dut);
    dut->i_init = 0;
}

// Feed bytes into the running remainder and return it. With `gap` set,
// each byte is followed by that many cycles of i_valid low with other data
// on i_data, as a receiver at 10 or 100 Mbit/s presents its bytes.
static uint32_t feed(Veth_crc32* dut, const std::vector<uint8_t>& data,
                     int gap = 0) {
    for (uint8_t b : data) {
        dut->i_valid = 1;
        dut->i_data = b;
        tick(dut);
        for (int g = 0; g < gap; g++) {
            dut->i_valid = 0;
            dut->i_data = (uint8_t)~b;
            tick(dut);
        }
    }
    dut->i_valid = 0;
    return dut->o_crc;
}

// The check sequence for a remainder, in the order its bytes are sent.
static std::vector<uint8_t> fcs_bytes(uint32_t remainder) {
    uint32_t fcs = ~remainder;
    return {(uint8_t)fcs, (uint8_t)(fcs >> 8), (uint8_t)(fcs >> 16),
            (uint8_t)(fcs >> 24)};
}

static uint32_t reverse_bits(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++)
        if (v & (1u << i))
            r |= 1u << (31 - i);
    return r;
}

// C++ reference: the division as IEEE 802.3 states it, the coefficient of
// x^31 in bit 31 and the register shifting up, each byte's bits taken least
// significant first. The module keeps the same remainder bit-reversed, so
// this is a second formulation and not a copy of its loop.
static uint32_t ref_crc32_remainder(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data) {
        for (int i = 0; i < 8; i++) {
            uint32_t feedback = ((crc >> 31) ^ (byte >> i)) & 1u;
            crc <<= 1;
            if (feedback)
                crc ^= 0x04C11DB7u;
        }
    }
    return reverse_bits(crc);
}

// The test set: directed edge cases, the shortest and longest frames
// without their check sequences, and a deterministic pseudo-random spread of
// lengths (fixed LCG seed, so runs are reproducible).
static std::vector<std::vector<uint8_t>> build_vectors() {
    std::vector<std::vector<uint8_t>> v;
    v.push_back({});                                 // no bytes -> seed
    v.push_back({0x00});
    v.push_back({0xff});
    v.push_back({0x01});                             // feedback at the first bit
    v.push_back({0x80});                             // feedback at the last bit
    v.push_back({0x00, 0x00, 0x00, 0x00});
    v.push_back({0xde, 0xad, 0xbe, 0xef});

    uint32_t lcg = 0x1234567u;
    for (int len : {60, 1514}) {
        std::vector<uint8_t> frame;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            frame.push_back((uint8_t)(lcg >> 24));
        }
        v.push_back(frame);
    }
    for (int n = 0; n < 256; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = (int)(lcg >> 26);                  // length 0..63
        std::vector<uint8_t> bytes;
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            bytes.push_back((uint8_t)(lcg >> 24));
        }
        v.push_back(bytes);
    }
    return v;
}

static int pass = 0, fail = 0;

static void check(bool ok, const char* what, size_t len, uint32_t want,
                  uint32_t got) {
    if (ok) {
        pass++;
    } else {
        printf("MISMATCH %s (len %zu): want %08x got %08x\n", what, len, want,
               got);
        fail++;
    }
}

int main() {
    Veth_crc32* dut = new Veth_crc32;

    dut->i_clk = 0;
    dut->i_init = 0;
    dut->i_valid = 0;
    dut->i_data = 0;

    // The published check value: the CRC of the ASCII digits 1 to 9, which
    // is the complement of the remainder.
    {
        std::vector<uint8_t> digits = {'1', '2', '3', '4', '5',
                                       '6', '7', '8', '9'};
        seed(dut);
        uint32_t res = feed(dut, digits);
        check(~res == 0xCBF43926u, "check value", digits.size(), 0xCBF43926u,
              ~res);
    }

    for (const auto& data : build_vectors()) {
        // The remainder over the bytes, against the reference.
        seed(dut);
        uint32_t res = feed(dut, data);
        uint32_t ref = ref_crc32_remainder(data);
        check(res == ref, "remainder", data.size(), ref, res);

        // On through the check sequence: an intact frame leaves the residue.
        uint32_t after = feed(dut, fcs_bytes(res));
        check(after == RESIDUE, "residue", data.size(), RESIDUE, after);

        // The same bytes with idle cycles between them, i_data changing
        // meanwhile: only a valid byte moves the remainder.
        seed(dut);
        uint32_t gapped = feed(dut, data, 1);
        check(gapped == ref, "remainder with gaps", data.size(), ref, gapped);

        // One bit wrong anywhere in the frame, and the residue is not left.
        if (!data.empty()) {
            std::vector<uint8_t> bad = data;
            bad[bad.size() / 2] ^= 0x10;
            seed(dut);
            feed(dut, bad);
            uint32_t bad_after = feed(dut, fcs_bytes(res));
            check(bad_after != RESIDUE, "corrupted frame keeps the residue",
                  data.size(), RESIDUE, bad_after);
        }
    }

    // i_init seeds whatever else is asked: with i_valid beside it the byte
    // is not folded in.
    {
        seed(dut);
        feed(dut, {0x12, 0x34});
        dut->i_init = 1;
        dut->i_valid = 1;
        dut->i_data = 0x56;
        tick(dut);
        dut->i_init = 0;
        dut->i_valid = 0;
        check(dut->o_crc == 0xFFFFFFFFu, "init beside valid", 0, 0xFFFFFFFFu,
              dut->o_crc);
    }

    printf("eth_crc32: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

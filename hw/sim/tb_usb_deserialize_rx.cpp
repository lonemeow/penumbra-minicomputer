// Verilator testbench for the Penumbra USB receive deserializer
//
// Mirror-checks usb_serialize_tx: serialize bytes to bits (reference, LSB
// first), stream them into the deserializer, and require the bytes back. Three
// things are exercised:
//   * i_valid gaps      -- the unstuffer's removed stuff 0s, which the
//     deserializer must skip without counting them as data;
//   * i_init alignment  -- a strobe before each packet sets byte boundaries;
//   * cross-packet realignment -- a dangling partial bit before i_init must not
//     misalign the following packet.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_deserialize_rx.h"

static void settle(Vusb_deserialize_rx* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_deserialize_rx* d)   { d->i_clk = 1; d->eval(); }

static std::vector<int> ref_bits(const std::vector<uint8_t>& bytes) {
    std::vector<int> o;
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; i++)
            o.push_back((b >> i) & 1);
    return o;
}

static void reset(Vusb_deserialize_rx* d) {
    d->i_rst = 1; d->i_init = 0; d->i_valid = 0; d->i_data_bit = 0;
    settle(d); edge(d); settle(d); edge(d);
    d->i_rst = 0;
}

// One alignment strobe (no data bit this cycle).
static void align(Vusb_deserialize_rx* d) {
    d->i_init = 1; d->i_valid = 0;
    settle(d); edge(d);
    d->i_init = 0;
}

// Stream a raw bit list, optionally inserting an idle (i_valid=0) gap every
// gap_period cycles to model removed stuff bits. Collects completed bytes.
static void stream_bits(Vusb_deserialize_rx* d, const std::vector<int>& bits,
                        int gap_period, std::vector<uint8_t>& out) {
    int c = 0;
    for (size_t i = 0; i < bits.size();) {
        bool gap = gap_period > 1 && (c % gap_period) == 0;
        d->i_valid    = gap ? 0 : 1;
        d->i_data_bit = gap ? 0 : bits[i];
        settle(d);
        if (d->o_byte_valid) out.push_back(d->o_byte);
        edge(d);
        if (!gap) i++;
        c++;
    }
    d->i_valid = 0;
}

static std::vector<uint8_t> recv_packet(Vusb_deserialize_rx* d,
                                        const std::vector<uint8_t>& bytes,
                                        int gap_period) {
    std::vector<uint8_t> out;
    align(d);
    stream_bits(d, ref_bits(bytes), gap_period, out);
    return out;
}

static std::vector<std::vector<uint8_t>> byte_streams() {
    std::vector<std::vector<uint8_t>> v = {
        {0xB4}, {0x00}, {0xFF}, {0x80, 0x01}, {0xD2, 0xC3, 0x00, 0xFF, 0xA5},
    };
    uint32_t lcg = 0xa5a51234u;
    for (int n = 0; n < 64; n++) {
        lcg = lcg * 1664525u + 1013904223u;
        int len = 1 + (int)((lcg >> 28) & 7);
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
    Vusb_deserialize_rx* dut = new Vusb_deserialize_rx;
    int pass = 0, fail = 0;

    const int gap_periods[] = {1, 2, 3, 5};   // 1 = no gaps

    // Per-packet recovery under several gap densities (fresh reset each run).
    for (const auto& bytes : byte_streams()) {
        for (int gp : gap_periods) {
            reset(dut);
            std::vector<uint8_t> got = recv_packet(dut, bytes, gp);
            if (got == bytes) {
                pass++;
            } else {
                printf("FAIL recover gap=%d on %zu bytes: got %zu\n",
                       gp, bytes.size(), got.size());
                fail++;
            }
        }
    }

    // Cross-packet realignment: a dangling partial bit (5 bits, not a whole
    // byte) before i_init must not misalign the next packet.
    {
        reset(dut);
        std::vector<uint8_t> pktA = {0x3C, 0x55};
        std::vector<uint8_t> pktB = {0x99, 0xE1, 0x07};

        std::vector<uint8_t> gotA = recv_packet(dut, pktA, 1);

        // dangle 5 stray valid bits with no alignment strobe
        std::vector<int> dangle = {1, 0, 1, 1, 0};
        std::vector<uint8_t> sink;
        stream_bits(dut, dangle, 1, sink);

        std::vector<uint8_t> gotB = recv_packet(dut, pktB, 1);

        if (gotA == pktA && gotB == pktB) {
            pass++;
        } else {
            printf("FAIL realignment: A %s, B %s\n",
                   (gotA == pktA) ? "ok" : "bad", (gotB == pktB) ? "ok" : "bad");
            fail++;
        }
    }

    printf("usb_deserialize_rx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

// Verilator testbench for the Penumbra USB receive-chain integration
//
// End-to-end: builds the actual D+/D- line waveform of complete packets --
// SYNC field, NRZI-encoded bit-stuffed payload, SE0 EOP -- clocks it into the
// composed receive chain at the oversample rate, and requires the payload
// bytes back out of the deserializer with one EOP per packet and no stuff
// errors. Covers both speeds, +/-1 clock edge jitter, hub-stripped SYNC,
// stuff-heavy payloads, and back-to-back packets whose tail 1-runs would
// corrupt the next packet if the unstuffer's count were not re-seeded.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_rx_test.h"

// Mirror usb_pkg encodings.
enum { SPEED_FS = 1, SPEED_LS = 2 };   // usb_pkg usb_speed_e (UTMI+ XcvrSelect)
static int div_of(int speed) { return speed == SPEED_LS ? 40 : 5; }

struct Pin { int dp, dn; };

static void settle(Vusb_rx_test* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_rx_test* d)   { d->i_clk = 1; d->eval(); }

static std::vector<int> bytes_to_bits(const std::vector<uint8_t>& bytes) {
    std::vector<int> out;
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; i++)
            out.push_back((b >> i) & 1);
    return out;
}

// Transmit-side stuffing, count seeded at 1 by SYNC's terminating 1 (the USB
// stuff count includes it -- same rule the unstuffer's i_init seeds).
static std::vector<int> stuff(const std::vector<int>& bits) {
    std::vector<int> out;
    int ones = 1;
    for (int b : bits) {
        out.push_back(b);
        if (b) { if (++ones == 6) { out.push_back(0); ones = 0; } }
        else ones = 0;
    }
    return out;
}

// One packet as a per-clock pin waveform: idle J, then the NRZI-encoded
// SYNC+payload symbols each held one bit period (interior boundaries jittered
// +/-1 clock when seeded), then a two-bit-time SE0 EOP and a return to idle J.
static std::vector<Pin> packet_wave(const std::vector<uint8_t>& payload,
                                    int speed, int sync_zeros, uint32_t jitter) {
    const int div = div_of(speed);
    const Pin J   = (speed == SPEED_LS) ? Pin{0, 1} : Pin{1, 0};
    const Pin K   = (speed == SPEED_LS) ? Pin{1, 0} : Pin{0, 1};
    const Pin SE0 = {0, 0};

    // SYNC bits (zeros then the terminating 1) + stuffed payload, NRZI-encoded
    // starting from the idle J level.
    std::vector<int> bits(sync_zeros, 0);
    bits.push_back(1);
    for (int b : stuff(bytes_to_bits(payload))) bits.push_back(b);
    std::vector<Pin> symbols;
    int level = 1;                      // idle J
    for (int b : bits) {
        if (!b) level ^= 1;             // NRZI: 0 toggles, 1 holds
        symbols.push_back(level ? J : K);
    }

    // Expand to clocks with jittered interior symbol boundaries.
    int n = (int)symbols.size();
    std::vector<int> bnd(n + 1);
    bnd[0] = 0; bnd[n] = n * div;
    uint32_t lcg = jitter;
    for (int i = 1; i < n; i++) {
        int j = 0;
        if (jitter) { lcg = lcg * 1664525u + 1013904223u; j = (int)((lcg >> 30) % 3) - 1; }
        bnd[i] = i * div + j;
    }
    std::vector<Pin> wave(2 * div, J);  // idle preamble
    for (int i = 0; i < n; i++)
        for (int t = bnd[i]; t < bnd[i + 1]; t++)
            wave.push_back(symbols[i]);
    for (int t = 0; t < 2 * div; t++) wave.push_back(SE0);   // EOP
    for (int t = 0; t < 4 * div; t++) wave.push_back(J);     // back to idle
    return wave;
}

struct Res { std::vector<uint8_t> bytes; int eop = 0; bool error = false; };

static Res run(Vusb_rx_test* dut, int speed,
               const std::vector<std::vector<uint8_t>>& packets,
               int sync_zeros, uint32_t jitter) {
    dut->i_rst = 1; dut->i_speed = speed;
    // idle line levels while in reset
    dut->i_dp = (speed == SPEED_LS) ? 0 : 1;
    dut->i_dn = (speed == SPEED_LS) ? 1 : 0;
    settle(dut); edge(dut); settle(dut); edge(dut);
    dut->i_rst = 0;

    Res r;
    for (const auto& payload : packets) {
        for (const Pin& p : packet_wave(payload, speed, sync_zeros, jitter)) {
            dut->i_dp = p.dp;
            dut->i_dn = p.dn;
            settle(dut);
            if (dut->o_byte_valid) r.bytes.push_back(dut->o_byte);
            if (dut->o_eop)        r.eop++;
            if (dut->o_error)      r.error = true;
            edge(dut);
        }
    }
    return r;
}

int main() {
    Vusb_rx_test* dut = new Vusb_rx_test;
    int pass = 0, fail = 0;

    struct Case {
        const char* name;
        int speed;
        int sync_zeros;
        uint32_t jitter;
        std::vector<std::vector<uint8_t>> packets;
    };
    std::vector<Case> cases = {
        {"FS single byte",     SPEED_FS, 7, 0,          {{0xC3}}},
        {"FS token-sized",     SPEED_FS, 7, 0,          {{0x2D, 0x10, 0xE0}}},
        {"FS jitter",          SPEED_FS, 7, 0xc0ffeeu,  {{0xB4, 0x5A, 0x00, 0xFF}}},
        {"FS stuff-heavy",     SPEED_FS, 7, 0,          {{0xFF, 0xFF, 0xFF}}},
        {"FS stuff at start",  SPEED_FS, 7, 0,          {{0x1F, 0xAA}}},
        {"FS stuff before EOP",SPEED_FS, 7, 0,          {{0xFC}}},
        {"FS hub-stripped",    SPEED_FS, 3, 0,          {{0x69, 0x96}}},
        {"FS b2b tail-ones",   SPEED_FS, 7, 0,          {{0xF8}, {0x33, 0xCC}}},
        {"FS b2b jitter",      SPEED_FS, 7, 0x0badf00du,{{0xE0, 0x07}, {0x81}, {0x7E, 0x55}}},
        {"LS single byte",     SPEED_LS, 7, 0,          {{0xC3}}},
        {"LS stuff-heavy",     SPEED_LS, 7, 0,          {{0xFF, 0x0F}}},
        {"LS jitter",          SPEED_LS, 7, 0xa11ce5u,  {{0x2D, 0x10}}},
    };

    // Random multi-packet sweeps at full speed.
    uint32_t lcg = 0x5eed0001u;
    for (int n = 0; n < 16; n++) {
        Case c{"FS random", SPEED_FS, 7, 0, {}};
        lcg = lcg * 1664525u + 1013904223u;
        int npkt = 1 + ((lcg >> 28) & 1);
        for (int p = 0; p < npkt; p++) {
            lcg = lcg * 1664525u + 1013904223u;
            int len = 1 + (int)((lcg >> 28) & 7);
            std::vector<uint8_t> bytes;
            for (int i = 0; i < len; i++) {
                lcg = lcg * 1664525u + 1013904223u;
                bytes.push_back((uint8_t)(lcg >> 24));
            }
            c.packets.push_back(bytes);
        }
        lcg = lcg * 1664525u + 1013904223u;
        c.jitter = (n & 1) ? lcg : 0;
        cases.push_back(c);
    }

    for (const auto& c : cases) {
        std::vector<uint8_t> expected;
        for (const auto& p : c.packets)
            expected.insert(expected.end(), p.begin(), p.end());

        Res r = run(dut, c.speed, c.packets, c.sync_zeros, c.jitter);
        if (r.bytes == expected && r.eop == (int)c.packets.size() && !r.error) {
            pass++;
        } else {
            printf("FAIL [%s]: bytes %zu/%zu eop %d/%zu error=%d\n",
                   c.name, r.bytes.size(), expected.size(),
                   r.eop, c.packets.size(), r.error);
            fail++;
        }
    }

    printf("usb_rx_test: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

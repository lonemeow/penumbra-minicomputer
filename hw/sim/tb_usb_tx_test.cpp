// Verilator testbench for the Penumbra USB transmit-chain integration
//
// Feeds packet bytes into the composed transmit chain and checks the raw
// (dp, dn, oe) waveform structurally against the USB line format:
//   * the first driven symbol starts SYNC: K J K J K J K K at bit-period
//     spacing;
//   * the payload symbols NRZI-decode and unstuff (count seeded by SYNC's 1)
//     back to the fed bytes, with the stuffed bit count exact — a dropped
//     trailing stuff bit (payload ending in six 1s) fails the length check;
//   * EOP is SE0 for two bit times, a driven J for one, then the drive
//     releases.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_tx_test.h"

enum { SPEED_FS = 0, SPEED_LS = 1 };
static int div_of(int speed) { return speed == SPEED_LS ? 40 : 5; }

enum { SYM_SE0 = 0, SYM_J = 1, SYM_K = 2, SYM_SE1 = 3 };

struct Clk { int dp, dn, oe; };

static void settle(Vusb_tx_test* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_tx_test* d)   { d->i_clk = 1; d->eval(); }

static int classify(int speed, int dp, int dn) {
    if (dp == dn) return dp ? SYM_SE1 : SYM_SE0;
    bool j = (speed == SPEED_LS) ? (dn == 1) : (dp == 1);
    return j ? SYM_J : SYM_K;
}

static std::vector<int> ref_bits(const std::vector<uint8_t>& bytes) {
    std::vector<int> out;
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; i++)
            out.push_back((b >> i) & 1);
    return out;
}

static std::vector<int> ref_stuff(const std::vector<int>& bits) {
    std::vector<int> out;
    int ones = 1;   // SYNC's ending 1 seeds the run
    for (int b : bits) {
        out.push_back(b);
        if (b) { if (++ones == 6) { out.push_back(0); ones = 0; } }
        else ones = 0;
    }
    return out;
}

// Feed one packet through the byte handshake and record the pin waveform
// until the drive releases (or a timeout, reported as an empty record).
static std::vector<Clk> transmit(Vusb_tx_test* dut, int speed,
                                 const std::vector<uint8_t>& bytes) {
    dut->i_rst = 1; dut->i_speed = speed;
    dut->i_byte = 0; dut->i_byte_valid = 0;
    settle(dut); edge(dut); settle(dut); edge(dut);
    dut->i_rst = 0;

    std::vector<Clk> wave;
    size_t idx = 0;
    bool oe_seen = false;
    long limit = (long)(bytes.size() * 8 + 32) * div_of(speed) * 4 + 200;
    for (long c = 0; c < limit; c++) {
        bool offer = (idx < bytes.size());
        dut->i_byte       = offer ? bytes[idx] : 0;
        dut->i_byte_valid = offer;
        settle(dut);
        if (dut->o_byte_ready && offer) idx++;
        wave.push_back({dut->o_dp, dut->o_dn, dut->o_oe});
        if (dut->o_oe) oe_seen = true;
        else if (oe_seen) return wave;                    // drive released: done
        edge(dut);
    }
    return {};                                            // never finished
}

static bool analyze(const char* name, int speed, const std::vector<uint8_t>& bytes,
                    const std::vector<Clk>& wave) {
    const int div = div_of(speed);
    if (wave.empty()) { printf("FAIL [%s]: packet never completed\n", name); return false; }

    // Locate the driven window.
    size_t rise = 0;
    while (rise < wave.size() && !wave[rise].oe) rise++;

    // Sample symbols mid-bit from the drive start until EOP's SE0 appears.
    std::vector<int> syms;
    for (size_t k = 0; ; k++) {
        size_t at = rise + k * div + div / 2;
        if (at >= wave.size() || !wave[at].oe) break;
        int s = classify(speed, wave[at].dp, wave[at].dn);
        if (s == SYM_SE0) break;
        syms.push_back(s);
    }

    // SYNC: KJKJKJKK.
    const int sync_want[8] = {SYM_K, SYM_J, SYM_K, SYM_J, SYM_K, SYM_J, SYM_K, SYM_K};
    bool sync_ok = syms.size() >= 8;
    for (int k = 0; sync_ok && k < 8; k++) sync_ok = (syms[k] == sync_want[k]);
    if (!sync_ok) { printf("FAIL [%s]: bad SYNC pattern\n", name); return false; }

    // Payload: NRZI-decode from SYNC's last symbol, then unstuff (seed 1).
    std::vector<int> line_bits;
    for (size_t k = 8; k < syms.size(); k++)
        line_bits.push_back(syms[k] == syms[k - 1]);      // no transition = 1
    std::vector<int> want_line = ref_stuff(ref_bits(bytes));
    if (line_bits != want_line) {
        printf("FAIL [%s]: line bits %zu/%zu (stuffing or payload wrong)\n",
               name, line_bits.size(), want_line.size());
        return false;
    }

    // EOP: two bit times of SE0, one of J, then release. The counts allow a
    // clock of skew from the registered pin stage.
    size_t p = rise;
    while (p < wave.size() && classify(speed, wave[p].dp, wave[p].dn) != SYM_SE0) p++;
    size_t se0 = 0;
    while (p + se0 < wave.size() && wave[p + se0].oe &&
           classify(speed, wave[p + se0].dp, wave[p + se0].dn) == SYM_SE0) se0++;
    size_t j = 0;
    while (p + se0 + j < wave.size() && wave[p + se0 + j].oe &&
           classify(speed, wave[p + se0 + j].dp, wave[p + se0 + j].dn) == SYM_J) j++;
    bool eop_ok = (se0 + 2 >= (size_t)(2 * div)) && (se0 <= (size_t)(2 * div) + 2)
               && (j + 2 >= (size_t)div) && (j <= (size_t)div + 2)
               && (p + se0 + j >= wave.size() - 2 || !wave[p + se0 + j].oe);
    if (!eop_ok) {
        printf("FAIL [%s]: EOP shape se0=%zu j=%zu (div=%d)\n", name, se0, j, div);
        return false;
    }
    return true;
}

int main() {
    Vusb_tx_test* dut = new Vusb_tx_test;
    int pass = 0, fail = 0;

    struct Case { const char* name; int speed; std::vector<uint8_t> bytes; };
    std::vector<Case> cases = {
        {"FS single byte",     SPEED_FS, {0xC3}},
        {"FS token-sized",     SPEED_FS, {0x2D, 0x10, 0xE0}},
        {"FS stuff-heavy",     SPEED_FS, {0xFF, 0xFF}},
        {"FS stuff at start",  SPEED_FS, {0x1F, 0xAA}},
        {"FS trailing stuff",  SPEED_FS, {0xFC}},
        {"FS zeros",           SPEED_FS, {0x00, 0x00}},
        {"LS single byte",     SPEED_LS, {0xC3}},
        {"LS trailing stuff",  SPEED_LS, {0xFC}},
    };
    uint32_t lcg = 0x7ea9001u;
    for (int n = 0; n < 8; n++) {
        Case c{"FS random", SPEED_FS, {}};
        lcg = lcg * 1664525u + 1013904223u;
        int len = 1 + (int)((lcg >> 28) & 7);
        for (int i = 0; i < len; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            c.bytes.push_back((uint8_t)(lcg >> 24));
        }
        cases.push_back(c);
    }

    for (const auto& c : cases) {
        std::vector<Clk> wave = transmit(dut, c.speed, c.bytes);
        (analyze(c.name, c.speed, c.bytes, wave) ? pass : fail)++;
    }

    printf("usb_tx_test: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

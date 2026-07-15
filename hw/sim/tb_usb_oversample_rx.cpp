// Verilator testbench for the Penumbra USB receive oversampler
//
// Builds an oversampled J/K line waveform from a known per-bit level sequence
// (each level held for the speed's divisor of 60 MHz clocks), feeds it to the
// sampler, and requires the recovered samples (o_line captured on o_bit_en) to
// reproduce the level sequence exactly -- one sample per bit, right value.
// Run at both speeds, with and without +/-1 clock edge jitter, and over the
// edge-free extremes (all-J, all-K) that exercise free-running between locks.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_oversample_rx.h"

// Speed codes mirror usb_pkg::usb_speed_e (UTMI+ XcvrSelect encoding).
static const int SPEED_FS = 1, SPEED_LS = 2;
static const int DIV_FS = 5, DIV_LS = 40;

static void settle(Vusb_oversample_rx* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_oversample_rx* d)   { d->i_clk = 1; d->eval(); }

// Expand a per-bit level sequence into a per-clock waveform: each bit spans
// `div` clocks, but a boundary may slip +/-1 clock when jitter is on, so edges
// land off the nominal grid the way a real device's clock would drift them.
static std::vector<int> build_wave(const std::vector<int>& levels, int div, uint32_t seed) {
    int n = (int)levels.size();
    std::vector<int> b(n + 1);
    b[0] = 0; b[n] = n * div;
    uint32_t lcg = seed;
    for (int i = 1; i < n; i++) {
        int j = 0;
        if (seed) { lcg = lcg * 1664525u + 1013904223u; j = (int)((lcg >> 30) % 3) - 1; }
        b[i] = i * div + j;     // gaps stay >= div-2 >= 3, so always increasing
    }
    std::vector<int> wave(n * div);
    for (int i = 0; i < n; i++)
        for (int t = b[i]; t < b[i + 1]; t++)
            wave[t] = levels[i];
    return wave;
}

static std::vector<int> run_sampler(Vusb_oversample_rx* dut, const std::vector<int>& levels,
                                    int speed, int div, uint32_t jitter_seed) {
    dut->i_rst = 1; dut->i_speed = speed; dut->i_line = 0;
    settle(dut); edge(dut); settle(dut); edge(dut);
    dut->i_rst = 0;

    std::vector<int> wave = build_wave(levels, div, jitter_seed);
    std::vector<int> recovered;
    for (int t = 0; t < (int)wave.size(); t++) {
        dut->i_line = wave[t];
        settle(dut);
        if (dut->o_bit_en) recovered.push_back(dut->o_line);
        edge(dut);
    }
    return recovered;
}

static std::vector<std::vector<int>> level_sequences(int len) {
    std::vector<std::vector<int>> v;
    v.push_back(std::vector<int>(len, 0));               // all-K: no edges, pure free-run
    v.push_back(std::vector<int>(len, 1));               // all-J
    std::vector<int> alt(len);
    for (int i = 0; i < len; i++) alt[i] = i & 1;        // edge every bit
    v.push_back(alt);
    uint32_t lcg = 0x13572468u;
    for (int n = 0; n < 16; n++) {
        std::vector<int> s(len);
        for (int i = 0; i < len; i++) { lcg = lcg * 1664525u + 1013904223u; s[i] = (lcg >> 31) & 1; }
        v.push_back(s);
    }
    return v;
}

int main() {
    Vusb_oversample_rx* dut = new Vusb_oversample_rx;
    int pass = 0, fail = 0;

    struct Run { const char* name; int speed; int div; int len; uint32_t jitter; };
    const Run runs[] = {
        {"FS clean",   SPEED_FS, DIV_FS, 32, 0},
        {"FS jitter",  SPEED_FS, DIV_FS, 32, 0xc0ffeeu},
        {"LS clean",   SPEED_LS, DIV_LS, 20, 0},
        {"LS jitter",  SPEED_LS, DIV_LS, 20, 0xa11ce5u},
    };

    for (const auto& r : runs) {
        for (const auto& levels : level_sequences(r.len)) {
            std::vector<int> got = run_sampler(dut, levels, r.speed, r.div, r.jitter);
            if (got == levels) {
                pass++;
            } else {
                printf("FAIL [%s] len=%d: recovered %zu of %zu\n",
                       r.name, r.len, got.size(), levels.size());
                fail++;
            }
        }
    }

    // Live speed switch: the port swaps the transceiver code between
    // packets (connect-time detect, the reset drive state) with no reset
    // in between. A long low-speed phase must not run off the shrunken
    // full-speed bit period — the sampler re-locks and the next packet
    // recovers in full.
    {
        dut->i_rst = 1; dut->i_speed = SPEED_LS; dut->i_line = 1;
        settle(dut); edge(dut); settle(dut); edge(dut);
        dut->i_rst = 0;
        // Idle J with no edges lets the phase counter run deep into the
        // low-speed bit before the switch.
        for (int t = 0; t < DIV_LS - 5; t++) {
            dut->i_line = 1;
            settle(dut); edge(dut);
        }
        dut->i_speed = SPEED_FS;
        for (int t = 0; t < 2 * DIV_FS; t++) {
            dut->i_line = 1;
            settle(dut); edge(dut);
        }
        std::vector<int> levels(16);
        for (int i = 0; i < 16; i++) levels[i] = (i >> 1) & 1;
        std::vector<int> wave = build_wave(levels, DIV_FS, 0);
        std::vector<int> recovered;
        for (int t = 0; t < (int)wave.size(); t++) {
            dut->i_line = wave[t];
            settle(dut);
            if (dut->o_bit_en) recovered.push_back(dut->o_line);
            edge(dut);
        }
        if (recovered == levels) {
            pass++;
        } else {
            printf("FAIL [LS->FS live switch]: recovered %zu of %zu\n",
                   recovered.size(), levels.size());
            fail++;
        }
    }

    printf("usb_oversample_rx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

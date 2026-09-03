// Ladder linearity sweep for the audio output stage.
//
// An investigation tool, not a regression test: it reports numbers
// rather than passing or failing. Results and the conclusions drawn
// from them are recorded in doc/TODO.md under the audio device entry.
//
// The modulator's code stream does not depend on what the ladder does
// with it, so one capture per stimulus level feeds every configuration
// and every parameter point. Each configuration names the ULX3S ladder
// legs its code drives; legs it does not drive are held low, so the
// ladder's Thevenin resistance stays code-independent.
//
// Stimulus is the CCIF two-tone pair. The difference product exists
// only through nonlinearity, which makes it far more sensitive than
// harmonic distortion on a single tone. What the sweep varies is the
// FPGA pin's own source and sink resistance: the board's ladder values
// compensate for a nominal 10 ohm, and a source/sink difference makes
// each leg's weight depend on its own state.

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <complex>
#include <vector>
#include "Vaudio_sigma_delta_test.h"

namespace {

// Ladder legs from the board schematic (R14-R17), index 0 = LSB.
const double R_LEG[4]  = {1100.0, 549.0, 270.0, 130.0};
constexpr double VDD    = 3.3;
constexpr double R_LOAD = 10000.0;    // line input

constexpr int    LOG2N = 21;
constexpr int    N     = 1 << LOG2N;
constexpr double FCLK  = 60e6;
constexpr int    OSR   = 1250;
constexpr double FS    = FCLK / OSR;
constexpr double BIN   = FCLK / N;

// Tones on exact bins of the modulator-rate transform, so the analysis
// needs no correction for where they land.
constexpr int BIN_F1 = 664, BIN_F2 = 699;
constexpr int BIN_IMD = BIN_F2 - BIN_F1;
constexpr int BAND_TOP = int(20000 / BIN);

typedef std::complex<double> cd;

struct Config {
    int         width;
    int         legs[4];    // ladder leg per code bit, MSB first
    const char* name;
};

const Config CONFIGS[] = {
    {2, {1,0},     "2b  549/1k1"},
    {3, {2,1,0},   "3b  270/549/1k1"},
    {3, {3,2,1},   "3b  130/270/549"},
    {4, {3,2,1,0}, "4b  all legs"},
    {1, {0},       "1b  1k1"},
    {1, {3},       "1b  130"},
};

// Source/sink resistance pairs. The board's values compensate for a
// nominal 10 ohm; the sweep asks what a departure from that costs.
struct PinR { double hi, lo; };
const PinR PINS[] = {
    {10.0, 10.0}, {10.0, 10.5}, {10.0, 11.0}, {10.0, 12.0}, {10.0, 15.0},
    { 5.0,  5.0}, {15.0, 15.0},
};

void fft(std::vector<cd>& a) {
    int n = a.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        cd wl(std::cos(-2 * M_PI / len), std::sin(-2 * M_PI / len));
        for (int i = 0; i < n; i += len) {
            cd w(1);
            for (int k = 0; k < len / 2; k++) {
                cd u = a[i+k], v = a[i+k+len/2] * w;
                a[i+k] = u + v;
                a[i+k+len/2] = u - v;
                w *= wl;
            }
        }
    }
}

// Ladder output for every pin pattern. Every leg is driven, so the
// Thevenin resistance moves only through the source/sink difference.
void ladder_lut(double rp_hi, double rp_lo, double lut[16]) {
    for (int c = 0; c < 16; c++) {
        double gsum = 0, num = 0;
        for (int i = 0; i < 4; i++) {
            bool hi = (c >> i) & 1;
            double g = 1.0 / (R_LEG[i] + (hi ? rp_hi : rp_lo));
            gsum += g;
            if (hi) num += g;
        }
        lut[c] = VDD * (num / gsum) * R_LOAD / (R_LOAD + 1.0 / gsum);
    }
}

int pins_for(const Config& cfg, int code) {
    int pins = 0;
    for (int j = 0; j < cfg.width; j++)
        if ((code >> (cfg.width - 1 - j)) & 1) pins |= 1 << cfg.legs[j];
    return pins;
}

} // namespace

int main() {
    Vaudio_sigma_delta_test* dut = new Vaudio_sigma_delta_test;

    // 4-term Blackman-Harris: -92 dB sidelobes, needed because the
    // products being measured sit 100 dB under the tones. Without a
    // window the modulator's state discontinuity across the transform
    // buries them in leakage.
    std::vector<double> win(N);
    for (int n = 0; n < N; n++) {
        double x = 2 * M_PI * n / N;
        win[n] = 0.35875 - 0.48829*std::cos(x) + 0.14128*std::cos(2*x)
                 - 0.01168*std::cos(3*x);
    }

    std::vector<uint8_t> code[5];
    for (int w = 1; w <= 4; w++) code[w].resize(N);
    std::vector<double> stim(N);

    for (double dbfs : {-6.0, -16.0}) {
        const double amp = 0.5 * 32767.0 * std::pow(10.0, dbfs / 20.0);

        dut->i_rst = 1; dut->i_sample = 0;
        for (int i = 0; i < 4; i++) {
            dut->i_clk = 0; dut->eval(); dut->i_clk = 1; dut->eval();
        }
        dut->i_rst = 0;

        for (long n = -100000; n < N; n++) {
            double t = double((n < 0 ? 0 : n) / OSR) / FS;
            dut->i_sample = static_cast<int16_t>(std::lround(
                amp * (std::sin(2*M_PI*BIN_F1*BIN*t) + std::sin(2*M_PI*BIN_F2*BIN*t))));
            dut->i_clk = 0; dut->eval();
            dut->i_clk = 1; dut->eval();
            if (n >= 0) {
                code[1][n] = dut->o_code1; code[2][n] = dut->o_code2;
                code[3][n] = dut->o_code3; code[4][n] = dut->o_code4;
                stim[n] = static_cast<int16_t>(dut->i_sample);
            }
        }

        printf("\n=== two-tone pair at %.0f dBFS ===\n", dbfs);
        printf("%18s %11s %6s  %8s %8s %8s %8s\n",
               "config", "pin hi/lo", "Vpp", "IMD", "THD+N", "DNL", "outband");

        // Everything the analysis needs from one transformed record.
        auto measure = [&](const std::vector<double>& v, double out[4]) {
            std::vector<cd> buf(N);
            double mean = 0;
            for (int n = 0; n < N; n++) mean += v[n];
            mean /= N;
            for (int n = 0; n < N; n++) buf[n] = (v[n] - mean) * win[n];
            fft(buf);

            auto band = [&](int k, int half) {
                double s = 0;
                for (int i = k - half; i <= k + half; i++) s += std::norm(buf[i]);
                return s;
            };
            double sig = band(BIN_F1, 5) + band(BIN_F2, 5);
            double nd = 0, oob = 0;
            for (int k = 6; k <= BAND_TOP; k++) {
                if (std::abs(k - BIN_F1) <= 5 || std::abs(k - BIN_F2) <= 5) continue;
                nd += std::norm(buf[k]);
            }
            for (int k = BAND_TOP + 1; k < N / 2; k++) oob += std::norm(buf[k]);
            out[0] = 10 * std::log10(band(BIN_IMD, 5) / sig);
            out[1] = 10 * std::log10(nd / sig);
            out[2] = 10 * std::log10(oob / sig);
        };

        // The 16-bit stimulus through a perfect converter: the floor
        // this measurement cannot see past, since STF is unity and the
        // input's own quantization reaches the output unshaped.
        double ref[4];
        measure(stim, ref);
        printf("%18s %11s %6s  %+8.1f %+8.1f %8s %+8.1f\n",
               "perfect DAC", "-", "-", ref[0], ref[1], "-", ref[2]);

        for (const Config& cfg : CONFIGS) {
            for (const PinR& pr : PINS) {
                double lut[16];
                ladder_lut(pr.hi, pr.lo, lut);

                int nlev = 1 << cfg.width;
                std::vector<double> curve(nlev);
                for (int c = 0; c < nlev; c++) curve[c] = lut[pins_for(cfg, c)];

                double lsb = (curve[nlev-1] - curve[0]) / (nlev - 1), dnl = 0;
                for (int c = 0; c + 1 < nlev; c++) {
                    double d = (curve[c+1] - curve[c]) / lsb - 1.0;
                    if (std::fabs(d) > std::fabs(dnl)) dnl = d;
                }

                std::vector<double> v(N);
                for (int n = 0; n < N; n++) v[n] = curve[code[cfg.width][n]];

                double m[4];
                measure(v, m);
                char pin[16];
                snprintf(pin, sizeof pin, "%.1f/%.1f", pr.hi, pr.lo);
                printf("%18s %11s %6.2f  %+8.1f %+8.1f %+8.3f %+8.1f\n",
                       cfg.name, pin, curve[nlev-1] - curve[0],
                       m[0], m[1], dnl, m[2]);
            }
        }
    }

    delete dut;
    return 0;
}

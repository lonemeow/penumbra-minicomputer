// Verilator testbench for the audio noise-shaping modulator
//
// Checks the three properties the output stage depends on, at every
// ladder width (OUT_W 1..4).
//
// The spectral tests window before transforming. The stimulus sits on
// an exact DFT bin, but the modulator's state does not return to its
// starting value at the end of the window, and a rectangular transform
// turns that discontinuity into a leakage skirt some 40 dB above the
// shaped noise it is meant to measure.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <vector>
#include "Vaudio_sigma_delta_test.h"

namespace {

constexpr int SAMPLE_W = 16;

// Spectral run length. The band edge at BAND_BINS makes the
// oversampling ratio N / (2 * BAND_BINS) = 64 — far below the
// hardware's 1250, but enough to separate shaped noise from flat.
constexpr int N_FFT     = 1 << 16;
constexpr int SIG_BIN   = 8;
constexpr int BAND_BINS = 512;
constexpr int NOISE_LO  = 16;   // clear of the windowed signal's skirt

struct Width {
    int out_w;
    uint32_t (*code)(Vaudio_sigma_delta_test*);   // one port per width
};

const Width WIDTHS[] = {
    {1, [](Vaudio_sigma_delta_test* d) -> uint32_t { return d->o_code1; }},
    {2, [](Vaudio_sigma_delta_test* d) -> uint32_t { return d->o_code2; }},
    {3, [](Vaudio_sigma_delta_test* d) -> uint32_t { return d->o_code3; }},
    {4, [](Vaudio_sigma_delta_test* d) -> uint32_t { return d->o_code4; }},
};
constexpr int N_WIDTHS = sizeof(WIDTHS) / sizeof(WIDTHS[0]);

void tick(Vaudio_sigma_delta_test* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

void reset(Vaudio_sigma_delta_test* dut) {
    dut->i_rst    = 1;
    dut->i_sample = 0;
    tick(dut);
    tick(dut);
    dut->i_rst = 0;
}

// The code a held input must average to: the RTL biases to offset
// binary, then scales by 1 - 2^-OUT_W to span the code range.
double expected_code(int16_t sample, int out_w) {
    uint32_t biased = static_cast<uint32_t>(sample + 32768);
    uint32_t scaled = biased - (biased >> out_w);
    return scaled / static_cast<double>(1u << (SAMPLE_W - out_w));
}

double mean_code(Vaudio_sigma_delta_test* dut, const Width& w,
                 int16_t sample, int n_warm, int n_meas) {
    dut->i_sample = sample;
    for (int i = 0; i < n_warm; i++) tick(dut);

    double sum = 0.0;
    for (int i = 0; i < n_meas; i++) {
        tick(dut);
        sum += w.code(dut);
    }
    return sum / n_meas;
}

double bin_power(const std::vector<double>& x, int k) {
    double re = 0.0, im = 0.0;
    const double step = 2.0 * M_PI * k / x.size();
    for (size_t n = 0; n < x.size(); n++) {
        re += x[n] * std::cos(step * n);
        im -= x[n] * std::sin(step * n);
    }
    return re * re + im * im;
}

// Mean bin power over [lo, 2*lo), sampled.
double octave_density(const std::vector<double>& x, int lo) {
    double acc = 0.0;
    int    cnt = 0;
    for (int k = lo; k < lo * 2; k += 4) { acc += bin_power(x, k); cnt++; }
    return acc / cnt;
}

int check(bool ok, int* pass, int* fail, const char* fmt, ...) {
    if (ok) { (*pass)++; return 0; }
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL [");
    vprintf(fmt, ap);
    printf("]\n");
    va_end(ap);
    (*fail)++;
    return 1;
}

} // namespace

int main() {
    Vaudio_sigma_delta_test* dut = new Vaudio_sigma_delta_test;
    int pass = 0, fail = 0;

    // ── Tracking ────────────────────────────────────────────────
    // The mean output follows a held input only if the loop closes.
    // A modulator whose feedback is miscomputed still emits plausible
    // switching output, so this is the test that says it is connected.
    // Levels sit off the code grid, where a truncating loop would bias.
    static const int16_t dc_levels[] = {
        0, 1000, -1000, 12345, -12345, 30000, -30000,
    };
    for (const Width& w : WIDTHS) {
        for (int16_t lvl : dc_levels) {
            reset(dut);
            double got  = mean_code(dut, w, lvl, 4096, 1 << 16);
            double want = expected_code(lvl, w.out_w);
            check(std::fabs(got - want) <= 0.01, &pass, &fail,
                  "track OUT_W=%d level=%d: mean code %.4f, expected %.4f",
                  w.out_w, lvl, got, want);
        }
    }

    // ── Shaping ─────────────────────────────────────────────────
    double snr_db[N_WIDTHS];
    for (int wi = 0; wi < N_WIDTHS; wi++) {
        const Width& w = WIDTHS[wi];
        reset(dut);

        // -6 dBFS keeps the loop clear of overload, where the quantizer
        // saturates and the measurement stops meaning anything.
        const double amp = 0.5 * 32767.0;
        std::vector<double> out(N_FFT);

        for (int n = 0; n < 4096; n++) {
            dut->i_sample = static_cast<int16_t>(
                std::lround(amp * std::sin(2.0 * M_PI * SIG_BIN * n / N_FFT)));
            tick(dut);
        }
        for (int n = 0; n < N_FFT; n++) {
            dut->i_sample = static_cast<int16_t>(
                std::lround(amp * std::sin(2.0 * M_PI * SIG_BIN * n / N_FFT)));
            tick(dut);
            out[n] = w.code(dut);
        }
        for (int n = 0; n < N_FFT; n++)
            out[n] *= 0.5 * (1.0 - std::cos(2.0 * M_PI * n / N_FFT));

        // Hann spreads the tone over three bins.
        double sig = 0.0;
        for (int k = SIG_BIN - 1; k <= SIG_BIN + 1; k++) sig += bin_power(out, k);

        double noise = 0.0;
        for (int k = NOISE_LO; k <= BAND_BINS; k++) noise += bin_power(out, k);
        snr_db[wi] = 10.0 * std::log10(sig / noise);

        // Octaves from 256 up. Below that the stimulus's own 16-bit
        // rounding dominates at the wider widths, and STF is unity, so
        // that noise reaches the output unshaped — a floor set by the
        // port width, not by the loop.
        double oct[4];
        for (int i = 0; i < 4; i++)
            oct[i] = 10.0 * std::log10(octave_density(out, 256 << i));

        printf("  OUT_W=%d  SNR %5.1f dB   octaves %.1f %.1f %.1f %.1f\n",
               w.out_w, snr_db[wi], oct[0], oct[1], oct[2], oct[3]);

        // (1 - z^-1)^2 puts noise power at f^4, so each octave up is
        // +12 dB. A first-order or mis-signed loop gives +6, a flat
        // (unshaped) one 0. The window is what makes this readable.
        for (int i = 1; i < 4; i++) {
            double slope = oct[i] - oct[i - 1];
            check(slope > 9.0 && slope < 15.0, &pass, &fail,
                  "slope OUT_W=%d octave %d: %+.1f dB, expected +12",
                  w.out_w, 256 << i, slope);
        }
    }

    // Absolute floors, roughly 8 dB under what a correct loop measures
    // here — enough to catch a structural regression without policing
    // the last few dB.
    static const double SNR_FLOOR[N_WIDTHS] = {60.0, 72.0, 80.0, 86.0};
    for (int wi = 0; wi < N_WIDTHS; wi++)
        check(snr_db[wi] >= SNR_FLOOR[wi], &pass, &fail,
              "SNR OUT_W=%d: %.1f dB below the %.0f dB floor",
              WIDTHS[wi].out_w, snr_db[wi], SNR_FLOOR[wi]);

    // A wider code quantizes more finely, so in-band noise has to fall
    // with the ladder width; a flat progression means OUT_W is not
    // reaching the quantizer.
    for (int wi = 1; wi < N_WIDTHS; wi++)
        check(snr_db[wi] > snr_db[wi - 1] + 2.0, &pass, &fail,
              "width scaling: OUT_W=%d SNR %.1f dB not above OUT_W=%d's %.1f dB",
              WIDTHS[wi].out_w, snr_db[wi], WIDTHS[wi - 1].out_w, snr_db[wi - 1]);

    // ── Overload ────────────────────────────────────────────────
    for (const Width& w : WIDTHS) {
        reset(dut);

        double got = mean_code(dut, w, 32767, 4096, 4096);
        double max = static_cast<double>((1u << w.out_w) - 1);
        check(got >= max - 0.01, &pass, &fail,
              "rail OUT_W=%d: mean code %.4f under full scale, expected %.4f",
              w.out_w, got, max);

        // Straight from the rail to midscale on a settle far shorter
        // than the tracking test's: a loop that wound up during
        // overload takes much longer than this to come back.
        got = mean_code(dut, w, 0, 256, 1 << 16);
        double want = expected_code(0, w.out_w);
        check(std::fabs(got - want) <= 0.01, &pass, &fail,
              "recover OUT_W=%d: mean code %.4f after overload, expected %.4f",
              w.out_w, got, want);
    }

    printf("\naudio_sigma_delta: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

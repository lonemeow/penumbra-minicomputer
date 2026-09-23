// Verilator testbench for a multi-channel CLASS_AUDIO device
//
// Covers what the channel count introduces and the single-channel
// testbench therefore cannot reach: that each channel's registers are
// its own, that the rate and gain a channel is walked with are the ones
// written to that channel, and that the channels sum.
//
// The gain checks are the load-bearing ones. A shared mixing datapath
// reads one channel's control values per pass, so a fault that routes
// the wrong channel's pair into the datapath is silent in every
// single-channel test and audible as the wrong instrument playing at
// the wrong volume in a real mix.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include "Vaudio_multi_test.h"

namespace {

constexpr uint32_t CAP = 0x000, CTRL = 0x008, CHAN_EN = 0x00C;
constexpr uint32_t UNDERRUN = 0x018, MASTER_VOL = 0x01C;
constexpr uint32_t C_CTRL = 0x00, C_INC = 0x04, C_VOL = 0x08;
constexpr uint32_t C_DEPTH = 0x0C, C_LEVEL = 0x10, C_WM = 0x14, C_FIFO = 0x18;

constexpr int DEPTH = 16, CHANNELS = 4;

// Per-channel register block: 0x100 + n * 0x20, from the specification.
constexpr uint32_t CH(int n, uint32_t r) { return 0x100 + n * 0x20 + r; }

// One distinguishable frame per channel, small enough that all four sum
// without approaching saturation.
constexpr uint32_t frame_of(int n) {
    return (uint32_t(0x0200 * (n + 1)) << 16) | uint32_t(0x0100 * (n + 1));
}

Vaudio_multi_test* dut;
std::vector<uint32_t> emitted;
int pass = 0, fail = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    va_list ap; va_start(ap, fmt);
    printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
    va_end(ap); fail++;
}

int phase = 0;
void step() {
    dut->i_aclk = 0; dut->eval();
    dut->i_aclk = 1; dut->eval();
    if (dut->o_frame_en)
        emitted.push_back((uint32_t(uint16_t(dut->o_frame_r)) << 16) |
                           uint16_t(dut->o_frame_l));
    if (++phase % 2 == 0) {
        dut->i_clk = 0; dut->eval();
        dut->i_clk = 1; dut->eval();
    }
}
void steps(int n) { for (int i = 0; i < n; i++) step(); }

// A simple bus device: the master holds its strobe through the ack
// cycle, which follows the first. A register edge is every other step.
void expect_ack(const char* what) {
    if (!dut->o_ack) {
        fail++;
        printf("FAIL: no ack in the cycle after a %s\n", what);
    }
}

void wr(uint32_t addr, uint32_t data) {
    dut->i_addr = addr; dut->i_wdata = data; dut->i_we = 1;
    steps(2);
    expect_ack("write");
    steps(2);
    dut->i_we = 0;
    steps(2);
}

uint32_t rd(uint32_t addr) {
    dut->i_addr = addr; dut->i_re = 1;
    steps(2);
    expect_ack("read");
    uint32_t v = dut->o_rdata;
    steps(2);
    dut->i_re = 0;
    steps(2);
    return v;
}

void reset() {
    dut->i_rst = 1; dut->i_arst = 1;
    dut->i_we = 0; dut->i_re = 0; dut->i_addr = 0; dut->i_wdata = 0;
    steps(8);
    dut->i_rst = 0; dut->i_arst = 0;
    steps(8);
    emitted.clear();
}

// Queue the same frame on every channel, so a channel that should be
// inaudible is silent because of its gain rather than because it has
// nothing to play.
void prime_all(int frames) {
    for (int c = 0; c < CHANNELS; c++)
        for (int i = 0; i < frames; i++) wr(CH(c, C_FIFO), frame_of(c));
}

} // namespace

int main() {
    dut = new Vaudio_multi_test;
    reset();

    // ── Discovery ────────────────────────────────────────────
    check(((rd(CAP) >> 16) & 0xFF) == CHANNELS,
          "CAP channels: %u, expected %d", (rd(CAP) >> 16) & 0xFF, CHANNELS);

    // ── Registers are per channel ────────────────────────────
    for (int c = 0; c < CHANNELS; c++) {
        wr(CH(c, C_INC), 0x00010000u + c);
        wr(CH(c, C_VOL), 0xAAAA0000u + c);
        wr(CH(c, C_WM),  0x00000004u + c);
    }
    for (int c = 0; c < CHANNELS; c++) {
        check(rd(CH(c, C_INC)) == 0x00010000u + c,
              "channel %d INC read %08X", c, rd(CH(c, C_INC)));
        check(rd(CH(c, C_VOL)) == 0xAAAA0000u + c,
              "channel %d VOL read %08X", c, rd(CH(c, C_VOL)));
        check(rd(CH(c, C_WM)) == 0x00000004u + c,
              "channel %d WM read %08X", c, rd(CH(c, C_WM)));
    }

    // Queues are independent: filling one leaves the others untouched.
    reset();
    for (int i = 0; i < 4; i++) wr(CH(2, C_FIFO), frame_of(2));
    for (int c = 0; c < CHANNELS; c++)
        check(rd(CH(c, C_LEVEL)) == uint32_t(c == 2 ? DEPTH - 4 : DEPTH),
              "channel %d LEVEL %u after filling only channel 2", c,
              rd(CH(c, C_LEVEL)));

    // ── Gain reaches the channel it was written to ───────────
    // One channel at unity and the rest silent: the mix is then exactly
    // that channel's frame, so a pair routed to the wrong slot shows up
    // as another channel's value.
    for (int audible = 0; audible < CHANNELS; audible++) {
        reset();
        for (int c = 0; c < CHANNELS; c++)
            wr(CH(c, C_VOL), c == audible ? 0xFFFFFFFFu : 0u);
        prime_all(8);
        wr(CHAN_EN, (1u << CHANNELS) - 1);
        wr(CTRL, 1);
        steps(64 * 6);
        check(!emitted.empty() && emitted.back() == frame_of(audible),
              "only channel %d at unity: output %08X, expected %08X",
              audible, emitted.empty() ? 0 : emitted.back(), frame_of(audible));
    }

    // ── Rate reaches the channel it was written to ───────────
    // Unity against half rate: the two queues must drain at different
    // speeds, which they cannot do if both are walked with one value.
    reset();
    wr(CH(0, C_INC), 0x00010000u);      // one source frame per output frame
    wr(CH(1, C_INC), 0x00008000u);      // one per two
    for (int i = 0; i < 8; i++) {
        wr(CH(0, C_FIFO), frame_of(0));
        wr(CH(1, C_FIFO), frame_of(1));
    }
    wr(CHAN_EN, 0x3);
    wr(CTRL, 1);
    steps(64 * 9);
    check(rd(CH(0, C_LEVEL)) == DEPTH,
          "unity-rate channel did not drain: LEVEL %u", rd(CH(0, C_LEVEL)));
    check(rd(CH(1, C_LEVEL)) < DEPTH,
          "half-rate channel drained as fast as the unity one: LEVEL %u",
          rd(CH(1, C_LEVEL)));

    // ── Channels sum ─────────────────────────────────────────
    reset();
    for (int c = 0; c < CHANNELS; c++)
        wr(CH(c, C_VOL), (c < 2) ? 0xFFFFFFFFu : 0u);
    prime_all(8);
    wr(CHAN_EN, (1u << CHANNELS) - 1);
    wr(CTRL, 1);
    steps(64 * 6);
    uint32_t want = ((uint32_t(0x0200 + 0x0400) << 16) | (0x0100 + 0x0200));
    check(!emitted.empty() && emitted.back() == want,
          "two channels at unity summed to %08X, expected %08X",
          emitted.empty() ? 0 : emitted.back(), want);

    // ── Underrun is reported per channel ─────────────────────
    // Fed with more frames than the run consumes, so the one channel
    // that stays quiet does so because it has data and not because the
    // run was short.
    reset();
    for (int i = 0; i < 12; i++) wr(CH(1, C_FIFO), frame_of(1));
    wr(CHAN_EN, (1u << CHANNELS) - 1);
    wr(CTRL, 1);
    steps(64 * 5);
    uint32_t un = rd(UNDERRUN);
    check((un & 0x2) == 0, "channel 1 underran although it was fed: %02X", un);
    check((un & 0xD) == 0xD, "empty channels did not report underrun: %02X", un);

    // Master volume applies to the sum, not to one channel.
    reset();
    wr(MASTER_VOL, 0);
    for (int c = 0; c < CHANNELS; c++) wr(CH(c, C_VOL), 0xFFFFFFFFu);
    prime_all(8);
    wr(CHAN_EN, (1u << CHANNELS) - 1);
    wr(CTRL, 1);
    steps(64 * 6);
    check(!emitted.empty() && emitted.back() == 0,
          "master volume at zero still emitted %08X", emitted.back());

    printf("\naudio multi-channel: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

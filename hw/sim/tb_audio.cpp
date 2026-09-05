// Verilator testbench for the CLASS_AUDIO device
//
// Checks the minimum protocol from doc/system/devices/audio.md: the
// registers a consumer reads to discover the device, the queue's
// occupancy contract, and the three behaviours a driver depends on
// being exact — that frames leave in order at the output rate, that
// UNDERRUN distinguishes "the audio sounds wrong" from "the device ran
// out of data", and that a full queue discards rather than corrupts.
//
// The two clock domains run at different rates here, so the frame
// handoff is exercised rather than assumed.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include "Vaudio_test.h"

namespace {

// Register offsets, from the device specification.
constexpr uint32_t CAP = 0x000, RATE = 0x004, CTRL = 0x008, CHAN_EN = 0x00C;
constexpr uint32_t IRQ_STATUS = 0x010, IRQ_ENABLE = 0x014, UNDERRUN = 0x018;
constexpr uint32_t MASTER_VOL = 0x01C;
constexpr uint32_t C_CTRL = 0x100, C_INC = 0x104, C_VOL = 0x108;
constexpr uint32_t C_DEPTH = 0x10C, C_LEVEL = 0x110, C_WM = 0x114, C_FIFO = 0x118;

constexpr int DEPTH = 16;

Vaudio_test* dut;
std::vector<uint32_t> emitted;   // frames seen leaving the output side
int pass = 0, fail = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    va_list ap; va_start(ap, fmt);
    printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
    va_end(ap); fail++;
}

// One output-domain edge; the register domain runs at half its rate, so
// the handoff crosses a real boundary rather than a shared edge.
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

void wr(uint32_t addr, uint32_t data) {
    // Hold across a register edge, which arrives every other step.
    dut->i_addr = addr; dut->i_wdata = data; dut->i_we = 1;
    steps(2);
    dut->i_we = 0;
    steps(2);
}

uint32_t rd(uint32_t addr) {
    // A read answers from a register a cycle after it is presented, so
    // hold the strobe until the device drops busy and sample then —
    // what the bus master does, and what sampling combinationally in
    // the presentation cycle hid.
    dut->i_addr = addr; dut->i_re = 1;
    do {
        steps(2);
    } while (dut->o_busy);
    uint32_t v = dut->o_rdata;
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

} // namespace

int main() {
    dut = new Vaudio_test;
    reset();

    // ── Discovery ────────────────────────────────────────────
    // A consumer reads these before touching anything else.
    check(rd(CAP) == 0x00010001, "CAP: %08X, expected version 1, 1 channel", rd(CAP));
    check(rd(RATE) == 48000, "RATE: %u, expected 48000", rd(RATE));
    check(rd(C_DEPTH) == DEPTH, "DEPTH: %u, expected %d", rd(C_DEPTH), DEPTH);
    check(rd(C_LEVEL) == DEPTH, "LEVEL at reset: %u, expected %d", rd(C_LEVEL), DEPTH);

    // Absent optional features read unity, so a consumer that never
    // inspects CAP still plays at the right rate and full volume.
    check(rd(C_INC) == 0x00010000, "INC: %08X, expected unity", rd(C_INC));
    check(rd(C_VOL) == 0xFFFFFFFF, "VOL: %08X, expected unity", rd(C_VOL));
    check(rd(MASTER_VOL) == 0xFFFFFFFF, "MASTER_VOL: %08X, expected unity", rd(MASTER_VOL));

    // Powers up silent, so it cannot emit whatever its FIFO holds.
    check(rd(CTRL) == 0, "CTRL at reset: %u, expected 0", rd(CTRL));
    check(rd(CHAN_EN) == 0, "CHAN_EN at reset: %u, expected 0", rd(CHAN_EN));

    // ── Queue occupancy ──────────────────────────────────────
    for (int i = 0; i < 4; i++) wr(C_FIFO, 0x11110000u + i);
    check(rd(C_LEVEL) == uint32_t(DEPTH - 4), "LEVEL after 4 frames: %u, expected %d",
          rd(C_LEVEL), DEPTH - 4);

    // A disabled device must not drain.
    steps(64 * 4);
    check(rd(C_LEVEL) == uint32_t(DEPTH - 4), "disabled device drained: LEVEL %u",
          rd(C_LEVEL));
    check(emitted.empty(), "disabled device emitted %zu frames", emitted.size());

    // Frames beyond LEVEL are discarded, not wrapped over live data.
    for (int i = 4; i < DEPTH + 6; i++) wr(C_FIFO, 0x11110000u + i);
    check(rd(C_LEVEL) == 0, "LEVEL when full: %u, expected 0", rd(C_LEVEL));

    // ── Draining, in order and at the output rate ────────────
    wr(CHAN_EN, 1);
    wr(CTRL, 1);
    steps(64 * (DEPTH + 8));

    // Enabling primes the prefetch, so the first frame out is the first
    // one queued; anything after the queue empties is the held repeat.
    bool ordered = emitted.size() >= size_t(DEPTH);
    for (size_t i = 0; ordered && i < size_t(DEPTH); i++)
        if (emitted[i] != 0x11110000u + i) ordered = false;
    check(ordered, "frames left out of order or short: %zu emitted, first %08X",
          emitted.size(), emitted.empty() ? 0 : emitted[0]);

    // ── Underrun ─────────────────────────────────────────────
    // The queue is empty and the channel is still enabled.
    check(rd(UNDERRUN) & 1, "UNDERRUN not set after the queue emptied");

    // Holding the last frame rather than substituting silence: a step to
    // zero is a click at exactly the moment something already went wrong.
    uint32_t held = emitted.back();
    steps(64 * 3);
    check(emitted.back() == held, "output did not hold its last frame: %08X vs %08X",
          emitted.back(), held);

    wr(UNDERRUN, 1);
    check((rd(UNDERRUN) & 1) == 0, "UNDERRUN did not clear on write-one");

    // ── Watermark interrupt ──────────────────────────────────
    reset();
    wr(C_WM, DEPTH / 2);
    wr(IRQ_ENABLE, 1);
    check(dut->o_irq == 1, "IRQ not asserted with an empty queue past the watermark");

    for (int i = 0; i < DEPTH; i++) wr(C_FIFO, i);
    wr(IRQ_STATUS, 1);
    check((rd(IRQ_STATUS) & 1) == 0, "IRQ_STATUS did not clear with the queue full");
    check(dut->o_irq == 0, "IRQ still asserted with the queue full");

    // A watermark the queue can never reach must never raise the flag.
    // The comparison is narrowed to the queue's width; the register is
    // not, so what was written still reads back.
    reset();
    wr(IRQ_ENABLE, 1);
    wr(C_WM, DEPTH);
    check(dut->o_irq == 1, "IRQ not asserted with WM exactly at DEPTH");

    reset();
    wr(IRQ_ENABLE, 1);
    wr(C_WM, DEPTH + 1);
    check(dut->o_irq == 0, "IRQ asserted for a watermark one past DEPTH");

    wr(C_WM, 0xDEADBEEFu);
    check(dut->o_irq == 0, "IRQ asserted for a far out-of-range watermark");
    check(rd(C_WM) == 0xDEADBEEFu, "WM read back %08X, expected DEADBEEF",
          rd(C_WM));

    // ── Flush ────────────────────────────────────────────────
    reset();
    for (int i = 0; i < DEPTH; i++) wr(C_FIFO, i);
    check(rd(C_LEVEL) == 0, "LEVEL before flush: %u, expected 0", rd(C_LEVEL));
    wr(C_CTRL, 1u << 8);
    check(rd(C_LEVEL) == DEPTH, "flush left %u free, expected %d", rd(C_LEVEL), DEPTH);

    printf("\naudio: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

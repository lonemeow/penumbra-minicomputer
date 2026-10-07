// Verilator testbench for the PHY's hardware reset
//
// Holds the sequencer to what a PHY and a board each rely on, in
// milliseconds worked out from the harness's clock rate and not in a
// count of cycles, so a wait rounded the wrong way shows:
//
//  * The pin is low from the first moment, through the board's reset,
//    and for no less than the PHY's reset time after it.
//  * Ready rises no less than the settling time after the pin does, and
//    stays up until the next board reset.
//  * Neither wait runs more than a few cycles over, so a sequence that
//    stalls shows as well as one that hurries.
//  * A board reset during either wait, or once ready, starts over: the
//    pin goes low at once and both waits are counted in full again.
//
// The board's reset lasts 10 ms at least (doc/hardware/bus-protocol.md,
// "Reset Timing"), and the resets here are that long. The sequencer does
// not rely on it; shorter ones are not a case.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include "Veth_phy_reset_test.h"

namespace {

Veth_phy_reset_test* dut;
int pass = 0, fail = 0;
long cycle = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL [cycle %ld: ", cycle); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 100) {
        printf("eth_phy_reset: gave up after %d failures\n", fail);
        exit(1);
    }
}

// As the harness has them.
const long CLK_HZ    = 102350;
const long RESET_MS  = 10;
const long SETTLE_MS = 30;

// Whether a number of cycles lasts a number of milliseconds, exactly.
bool lasts(long cycles, long ms) { return cycles * 1000 >= ms * CLK_HZ; }

// The fewest cycles that do.
long cycles_for(long ms) { return (ms * CLK_HZ + 999) / 1000; }

// How far over its least a wait may run: a register or two between the
// count and an output, and no more.
const long SLACK = 4;

// The board's reset, at its shortest.
const long BOARD_RESET = cycles_for(10);

bool pin_was = false;       // the pin through the cycle before
long rst_ended = -1;        // the first cycle with the board's reset over, or -1 while it is on
long pin_rose = -1;         // the cycle the pin rose in, or -1 while it is low
long ready_rose = -1;       // the cycle ready rose in, or -1 while it is clear

// One cycle with the board's reset as given.
void tick(bool rst) {
    dut->i_rst = rst;
    dut->i_clk = 1;
    dut->eval();
    dut->i_clk = 0;
    dut->eval();
    cycle++;

    bool pin = dut->o_phy_rst_n, ready = dut->o_ready;

    if (rst) {
        check(!pin, "the pin is high in the board's reset");
        check(!ready, "ready is set in the board's reset");
        rst_ended = pin_rose = ready_rose = -1;
        pin_was = pin;
        return;
    }
    if (rst_ended < 0)
        rst_ended = cycle;

    check(!ready || pin, "ready is set with the pin low");

    if (pin && !pin_was) {
        // Low since the reset ended, the cycle it rose in not among them.
        long low = cycle - rst_ended;
        check(lasts(low, RESET_MS),
              "the pin rose %ld cycles after the board's reset, short of %ld ms (%ld cycles)",
              low, RESET_MS, cycles_for(RESET_MS));
        check(low <= cycles_for(RESET_MS) + SLACK,
              "the pin rose %ld cycles after the board's reset: %ld ms is %ld",
              low, RESET_MS, cycles_for(RESET_MS));
        pin_rose = cycle;
    }
    if (!pin && pin_was)
        check(false, "the pin fell with no board reset");

    if (ready && ready_rose < 0) {
        check(pin_rose >= 0, "ready rose before the pin did");
        long settled = cycle - pin_rose;
        check(lasts(settled, SETTLE_MS),
              "ready rose %ld cycles after the pin, short of %ld ms (%ld cycles)",
              settled, SETTLE_MS, cycles_for(SETTLE_MS));
        check(settled <= cycles_for(SETTLE_MS) + SLACK,
              "ready rose %ld cycles after the pin: %ld ms is %ld",
              settled, SETTLE_MS, cycles_for(SETTLE_MS));
        ready_rose = cycle;
    }
    if (!ready && ready_rose >= 0)
        check(false, "ready fell with no board reset");

    pin_was = pin;
}

void run(long cycles, bool rst) {
    for (long i = 0; i < cycles; i++)
        tick(rst);
}

// A board reset, and then the sequence to its end.
void reset_and_finish(const char* what) {
    run(BOARD_RESET, true);
    run(cycles_for(RESET_MS) + cycles_for(SETTLE_MS) + 4 * SLACK, false);
    check(pin_rose >= 0, "%s: the pin never rose", what);
    check(ready_rose >= 0, "%s: ready never rose", what);
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_phy_reset_test;

    // As the FPGA configures it, before any clock.
    dut->i_clk = 0;
    dut->i_rst = 1;
    dut->eval();
    check(!dut->o_phy_rst_n, "the pin is high before the clock has run");

    reset_and_finish("from configuration");

    // Ready holds.
    run(3 * cycles_for(SETTLE_MS), false);
    check(dut->o_ready && dut->o_phy_rst_n, "ready did not hold");

    reset_and_finish("a board reset once ready");

    // A board reset in the middle of the pin's low time: the whole of it
    // is counted again, not what was left.
    run(BOARD_RESET, true);
    run(cycles_for(RESET_MS) / 2, false);
    check(!dut->o_phy_rst_n, "the pin rose half way through its low time");
    reset_and_finish("a board reset in the pin's low time");

    // And in the middle of the settling time.
    run(BOARD_RESET, true);
    run(cycles_for(RESET_MS) + cycles_for(SETTLE_MS) / 2, false);
    check(dut->o_phy_rst_n && !dut->o_ready,
          "half way through settling, the pin is %d and ready %d",
          dut->o_phy_rst_n, dut->o_ready);
    reset_and_finish("a board reset in the settling time");

    // A board reset the cycle ready rises.
    run(BOARD_RESET, true);
    while (!dut->o_ready && cycle < 1000000)
        tick(false);
    reset_and_finish("a board reset as ready rises");

    printf("eth_phy_reset: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

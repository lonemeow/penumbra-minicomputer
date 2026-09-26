// tb_bus_wait_meter — the longest wait one slave has made an access take.
//
// Drives bus_wait_meter (WIDTH 8) the way a slave's strobe and ack go on
// the bus, and checks the reading against the wait bus_response_fabric's
// watchdog would count for the same access: the cycles the access is
// presented without an ack, not counting the ack's own cycle.
//
// Covers:
//  - nothing measured before the first access;
//  - an access answered in its first cycle (a wait of zero);
//  - longer waits raising the reading and shorter ones leaving it;
//  - idle cycles between accesses counting toward nothing;
//  - a strobe held through an ack into the next access, which starts a
//    fresh wait, as the watchdog's does, rather than adding to the last;
//  - saturation at all-ones for a wait past the width.
#include "Vbus_wait_meter.h"
#include "verilated.h"
#include <cstdio>
#include <string>

static Vbus_wait_meter* d;
static int checks = 0, fails = 0;

static void chk(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: %s\n", what.c_str());
    }
}

static void tick() {
    d->i_clk = 0;
    d->eval();
    d->i_clk = 1;
    d->eval();
}

// One access that waits `wait` cycles, then is acked; the strobe drops
// after the ack cycle unless `held` keeps it up for the next access.
static void access(int wait, bool held = false) {
    d->i_req = 1;
    d->i_ack = 0;
    for (int i = 0; i < wait; i++)
        tick();
    d->i_ack = 1;
    tick();
    d->i_ack = 0;
    d->i_req = held;
}

static void idle(int cycles) {
    d->i_req = 0;
    d->i_ack = 0;
    for (int i = 0; i < cycles; i++)
        tick();
}

static void expect_max(unsigned want, const char* what) {
    char msg[128];
    snprintf(msg, sizeof msg, "%s: reads %u, want %u", what, (unsigned)d->o_max, want);
    chk(d->o_max == want, msg);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Vbus_wait_meter;
    d->i_req = 0;
    d->i_ack = 0;
    d->eval();

    idle(5);
    expect_max(0, "before any access");

    access(0);
    expect_max(0, "an access acked in its first cycle");

    access(5);
    expect_max(5, "a wait of 5");

    idle(40);
    expect_max(5, "idle cycles");

    access(3);
    expect_max(5, "a shorter wait after a longer one");

    access(9);
    expect_max(9, "a longer wait");

    // Back to back: the strobe stays up through the ack into the next
    // access, whose wait starts over.
    access(7, true);
    access(8, true);
    access(2);
    expect_max(9, "back-to-back accesses, each shorter than the longest");
    access(6, true);
    access(12);
    expect_max(12, "back-to-back: the second's own wait, not the sum");

    access(300);
    expect_max(255, "a wait past the width saturates");
    access(1);
    expect_max(255, "the saturated reading holds");

    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    delete d;
    return fails ? 1 : 0;
}

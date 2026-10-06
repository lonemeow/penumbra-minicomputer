// Verilator testbench for the Ethernet adapter's clock-domain crossing
//
// Two unrelated clocks, at ratios from a device clock three times the bus
// clock to one a twentieth of it, and a device clock that can stop. The
// checks are what the adapter's two sides rely on:
//
//  * Down: every bundle the device side holds is one the bus side
//    offered, whole, never older than the last, and the latest once the
//    bus side has left it alone a while.
//  * Up: the count the bus side reads never runs ahead of the device's or
//    back from its own, follows within a few cycles, and matches at rest,
//    across the count's wrap; pulses arrive one for one.
//  * Reset: after a reset of the bus side, however soon after the last,
//    the device side is reset too, and until it has been the bus side
//    reads a count of zero and no pulse. A device clock stopped through
//    the reset changes nothing but when.
//
// The machine's reset lasts 10 ms at least (doc/hardware/bus-protocol.md,
// "Reset Timing"): hundreds of thousands of cycles, where the crossing
// takes a handful. What the module relies on is only that it outlasts the
// crossing, so to keep the run short the resets here are some tens of
// times the crossing's length and no more. Shorter ones are not a case.
//
// A zero-delay simulation takes every bit of a crossing at one instant, so
// it cannot see a synchroniser capture a value in the middle of changing.
// That is the gate-level harness's ground (hw/sim/gate).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include "Veth_cdc.h"

namespace {

Veth_cdc* dut;
int pass = 0, fail = 0;
long now = 0;                       // time, in units a clock period counts

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL [t=%ld: ", now); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 500) {
        printf("eth_cdc: gave up after %d failures\n", fail);
        exit(1);
    }
}

uint32_t lcg = 0x1234567u;
uint32_t next_random() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

// ── The clocks ───────────────────────────────────────────────
long bus_period = 27, dev_period = 8;
long next_bus = 0, next_dev = 0;
bool dev_running = true;

// ── The bus side: what software and the register block do ────
// A bundle carries a sequence number and its complement, so one made of
// two bundles' bits shows.
uint32_t bundle(uint32_t n) { return ((~n & 0xffffu) << 16) | (n & 0xffffu); }

uint32_t down_n = 1;                // the number of the bundle on offer
bool     down_moving = true;        // offer a new bundle every so often
int      rst_cycles = 0;            // bus cycles of reset still to drive
long     last_rst_start = -1;       // when the latest reset began

// ── The device side: a receiver or transmitter ───────────────
long     dev_count = 0;             // frames counted since that side's reset
int      dev_pulses = 0;            // pulses sent since then
int      pulse_spacing = 4;         // device cycles between pulses, at least
int      since_pulse = 0;
bool     dev_moving = true;         // count and pulse while enabled
long     last_dev_rst = -2;         // the latest device clock edge spent in reset
uint32_t taken_n = 0;               // the number of the latest bundle taken
bool     taken_any = false;
bool     dev_was_in_rst = false;    // the device clock edge before this one was in reset
std::deque<std::pair<long, long>> dev_history;    // (time, dev_count)

// ── What the bus side has seen come up ───────────────────────
long shown_count = 0;               // the count read, unwrapped
int  bus_pulses = 0;

// How far behind the device's count the bus side's may run: out of the
// device's Gray register, through two synchroniser stages and the output
// register.
long lag() { return 2 * dev_period + 5 * bus_period; }

bool awaiting_dev_reset() { return last_dev_rst < last_rst_start; }

void bus_edge() {
    dut->i_clk = 1;
    dut->eval();
    dut->i_clk = 0;
    dut->eval();

    uint16_t up = dut->o_up_count;
    if (awaiting_dev_reset() || dut->i_rst) {
        // What the device side holds is from before the reset.
        check(up == 0 && !dut->o_up_pulse, "count %u, pulse %d before the device side reset",
              up, dut->o_up_pulse);
    } else {
        long count = shown_count + (uint16_t)(up - (uint16_t)shown_count);
        // A count read just behind the device side's reset that is not the
        // device's is one from before it: the bus side took the reset as
        // over while it was still under way. Nothing after that means
        // anything, so it ends the run.
        if (count > dev_count && now - last_dev_rst <= lag() + 6 * bus_period) {
            check(false,
                  "the bus side read count %u, from before the device side's reset, "
                  "%ld bus cycles after its own reset began: it stopped waiting while "
                  "the device side was still resetting. Did the request fall before "
                  "rst_busy rose?", up, (now - last_rst_start) / bus_period);
            printf("eth_cdc: the reset was taken as over too soon, so nothing past it "
                   "was tried (%d checks passed, %d failed)\n", pass, fail);
            exit(1);
        }
        check(count <= dev_count, "count read %ld, the device's is %ld", count, dev_count);
        while (dev_history.size() > 1 && dev_history[1].first <= now - lag())
            dev_history.pop_front();
        if (!dev_history.empty() && dev_history.front().first <= now - lag())
            check(count >= dev_history.front().second,
                  "count read %ld, the device's was %ld a while ago", count,
                  dev_history.front().second);
        shown_count = count;
        if (dut->o_up_pulse) bus_pulses++;
        check(bus_pulses <= dev_pulses, "%d pulses up for %d sent", bus_pulses, dev_pulses);
    }

    // The next cycle's inputs.
    if (rst_cycles > 0) {
        if (!dut->i_rst) last_rst_start = now;
        dut->i_rst = 1;
        dut->i_down = 0;
        rst_cycles--;
    } else {
        dut->i_rst = 0;
        if (down_moving && next_random() % 12 == 0) down_n++;
        dut->i_down = bundle(down_n);
    }
}

void dev_edge() {
    dut->i_dev_clk = 1;
    dut->eval();
    dut->i_dev_clk = 0;
    dut->eval();

    if (dut->o_dev_rst) {
        last_dev_rst = now;
        dev_count = 0;
        dev_pulses = bus_pulses = 0;
        shown_count = 0;
        since_pulse = 0;
        taken_any = false;
        dev_history.clear();
        // The reset takes a cycle to clear what the domain holds.
        if (dev_was_in_rst)
            check(dut->o_down == 0, "bundle %08x held in reset", dut->o_down);
        dev_was_in_rst = true;
        dut->i_up_count = 0;
        dut->i_up_pulse = 0;
        return;
    }
    dev_was_in_rst = false;

    uint32_t down = dut->o_down;
    if (down != 0) {
        uint32_t n = down & 0xffffu;
        check(down == bundle(n), "bundle %08x is not one that was offered", down);
        check((uint16_t)(down_n - n) < 0x8000u, "bundle %u taken, %u is the latest offered",
              n, down_n & 0xffffu);
        if (taken_any)
            check((uint16_t)(n - taken_n) < 0x8000u, "bundle %u taken after %u", n, taken_n);
        taken_n = n;
        taken_any = true;
    }

    // Like a receiver with its enable in the bundle: nothing moves until
    // one has come down.
    dut->i_up_pulse = 0;
    since_pulse++;
    if (down != 0 && dev_moving) {
        if (next_random() % 2) dev_count++;
        if (since_pulse >= pulse_spacing && next_random() % 3 == 0) {
            dut->i_up_pulse = 1;
            dev_pulses++;
            since_pulse = 0;
        }
    }
    dut->i_up_count = (uint16_t)dev_count;
    dev_history.push_back({now, dev_count});
}

// Run both clocks to a time, whichever edge is due first.
void run_until(long until) {
    while (true) {
        bool dev_due = dev_running && next_dev <= next_bus;
        long t = dev_due ? next_dev : next_bus;
        if (t > until) break;
        now = t;
        if (dev_due) { dev_edge(); next_dev += dev_period; }
        else         { bus_edge(); next_bus += bus_period; }
    }
    now = until;
    if (!dev_running && next_dev < now) next_dev = now + 1;
}

void run_bus_cycles(int n) { run_until(now + n * bus_period); }

// A reset has had time to cross, be taken and be let go: the device side
// has been through it and is out again. Nothing else can work until it
// is, so a reset that has not finished ends the run.
void expect_reset_done(const char* when) {
    check(!awaiting_dev_reset(),
          "%s: the device side never reset: the request did not reach it, or fell "
          "before it was taken", when);
    bool held = dut->o_dev_rst;
    check(!held,
          "%s: the device side is still in reset %ld bus cycles after the bus side's "
          "began: the request never fell", when,
          (now - last_rst_start) / bus_period);
    if (held || awaiting_dev_reset()) {
        printf("eth_cdc: the reset never completed, so nothing past it was tried "
               "(%d checks passed, %d failed)\n", pass, fail);
        exit(1);
    }
}

// Leave everything alone long enough for both sides to agree, and see
// that they do.
void settle_and_compare(const char* when) {
    down_moving = false;
    dev_moving = false;
    run_until(now + 10 * dev_period + 12 * bus_period);
    expect_reset_done(when);
    check(dut->o_down == dut->i_down,
          "%s: the device side holds bundle %08x, the bus side has offered %08x "
          "since %ld device cycles ago", when, dut->o_down, dut->i_down,
          (10 * dev_period + 12 * bus_period) / dev_period);
    check((uint16_t)dut->o_up_count == (uint16_t)dev_count, "%s: count read %u, the device's is %u",
          when, (uint16_t)dut->o_up_count, (uint16_t)dev_count);
    check(bus_pulses == dev_pulses, "%s: %d pulses up for %d sent", when, bus_pulses,
          dev_pulses);
    down_moving = true;
    dev_moving = true;
}

void reset_bus(int cycles) {
    rst_cycles = cycles;
    run_bus_cycles(cycles + 1);
}

// Time for a reset to cross, be taken and be let go, with room to spare.
long reset_time() { return 2 * (10 * dev_period + 10 * bus_period); }

// Bus cycles the machine's reset is held for: many times the crossing.
int reset_cycles() { return (int)(10 * reset_time() / bus_period); }

void set_clocks(long bus, long dev) {
    bus_period = bus;
    dev_period = dev;
    next_bus = now + 1;
    next_dev = now + 2;
    // Pulses further apart than the bus side takes to see one.
    pulse_spacing = (int)(6 * bus / dev) + 4;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_cdc;
    dut->i_clk = 0;
    dut->i_dev_clk = 0;
    dut->i_rst = 0;
    dut->i_down = 0;
    dut->i_up_count = 0;
    dut->i_up_pulse = 0;
    dut->eval();

    // (bus period, device period): a gigabit receive clock against a slow
    // bus, a 100 Mbit/s one against a fast bus, a 10 Mbit/s one, two clocks
    // all but equal, and a transmit clock.
    const long clocks[][2] = {{27, 8}, {17, 40}, {17, 400}, {40, 41}, {25, 10}};

    for (const auto& c : clocks) {
        set_clocks(c[0], c[1]);

        // Power-on, then traffic both ways.
        reset_bus(reset_cycles());
        run_until(now + reset_time());
        expect_reset_done("power-on");
        run_until(now + 3000 * bus_period);
        settle_and_compare("after traffic");

        // A reset in the middle of traffic, at different moments of it.
        for (int i = 0; i < 5; i++) {
            run_until(now + (300 + 37 * i) * bus_period);
            reset_bus(reset_cycles());
            run_until(now + reset_time());
            expect_reset_done("a reset mid-traffic");
            run_until(now + 400 * bus_period);
            settle_and_compare("after a reset mid-traffic");
        }

        // A second reset close behind the first, at every distance: the
        // reset button pressed again as the first reset is being let go.
        int sweep = (int)(reset_time() / bus_period) + 20;
        for (int gap = 0; gap < sweep; gap++) {
            reset_bus(reset_cycles());
            run_bus_cycles(gap);
            reset_bus(reset_cycles());
            run_until(now + reset_time());
            check(!awaiting_dev_reset(),
                  "second reset %d cycles behind the first: the device side missed it", gap);
            expect_reset_done("two resets close together");
            run_until(now + 60 * bus_period + 20 * dev_period);
            settle_and_compare("after two resets close together");
        }

        // The device clock stopped through a reset and for a while after:
        // the bus side reads zero and waits, and the reset is taken when the
        // clock returns.
        run_until(now + 200 * bus_period);
        dev_running = false;
        reset_bus(reset_cycles());
        run_bus_cycles(500);
        check(awaiting_dev_reset(), "the device side reset with its clock stopped");
        dev_running = true;
        next_dev = now + 3;
        run_until(now + reset_time());
        expect_reset_done("the device clock back after a reset");
        run_until(now + 400 * bus_period);
        settle_and_compare("after a reset with the device clock stopped");

        // The device clock stopped as a reset begins and back before it
        // ends: taken within the reset, like any other.
        run_until(now + 200 * bus_period);
        dev_running = false;
        rst_cycles = reset_cycles();
        run_bus_cycles(reset_cycles() / 2);
        dev_running = true;
        next_dev = now + 3;
        run_bus_cycles(reset_cycles() / 2 + 2);
        run_until(now + reset_time());
        expect_reset_done("the device clock back within a reset");
        run_until(now + 400 * bus_period);
        settle_and_compare("after a reset the device clock came back within");
    }

    // The count's wrap, at the fastest device clock.
    {
        set_clocks(27, 8);
        reset_bus(reset_cycles());
        run_until(now + reset_time());
        expect_reset_done("before the count's wrap");
        while (dev_count < 70000 && now < 4000000000L)
            run_until(now + 5000 * bus_period);
        check(dev_count >= 70000, "the device's count never reached the wrap");
        settle_and_compare("past the count's wrap");
    }

    printf("eth_cdc: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

// Verilator testbench for the Ethernet PHY management master
//
// The master drives a modelled management bus: its clock and data pins,
// the pull-up on the data pin, a PHY that follows the pins as clause 22
// has one do, and a monitor that holds everything the master draws to
// the minimums a PHY asks for. The checks are the ones the adapter's
// MDIO register depends on: a write reaches the PHY whole, a read comes
// back whole however late in its allowance the PHY answers, an address
// nothing answers at reads as all ones, the two never drive the data pin
// together, and a start is one frame, held from the moment it was given.

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include "Veth_mdio_test.h"

namespace {

Veth_mdio_test* dut;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
}

// The harness runs the master on 25 MHz, so a cycle is 40 ns.
constexpr int NS_PER_CYCLE = 40;
constexpr long cycles(int ns) { return (ns + NS_PER_CYCLE - 1) / NS_PER_CYCLE; }

// What a PHY asks of the management pins, in ns (RTL8211E datasheet,
// "MDC/MDIO Timing"; clause 22 asks the same).
constexpr int T_MDC_HIGH   = 160;
constexpr int T_MDC_LOW    = 160;
constexpr int T_MDC_PERIOD = 400;
constexpr int T_SETUP      = 10;    // data pin settled before MDC rises
constexpr int T_HOLD       = 10;    // and held after
constexpr int T_VALID      = 300;   // MDC's rise to the PHY's answer, at most

// The contract's bound on a transaction (doc/system/devices/ethernet.md).
constexpr int T_BUSY_NS = 1000000;

constexpr int PHY_ADDR   = 1;
constexpr int FRAME_BITS = 64;

// The 32 bits after the preamble, as the master should send them.
uint32_t frame_word(bool write, int phy_addr, int reg_addr, uint16_t data) {
    return (0x1u << 30) | ((write ? 0x1u : 0x2u) << 28) |
           ((uint32_t)phy_addr << 23) | ((uint32_t)reg_addr << 18) |
           (0x2u << 16) | data;
}

// ── The PHY ──────────────────────────────────────────────────
// Takes the data pin on each rise of MDC, wants 32 ones before a frame,
// and answers a read at its own address from the turnaround on, moving
// the pin valid_delay cycles after the rise it answers from.
struct Phy {
    uint16_t regs[32] = {};
    int      valid_delay = 0;

    bool oe = false, val = true;            // what it puts on the pin
    struct Change { long at; bool oe, val; };
    std::deque<Change> pending;

    int      ones = 0, driven_ones = 0;     // run of ones; of ones the master drove
    bool     in_frame = false;
    int      nbit = 0;                      // bits of the word taken so far
    uint32_t word = 0;
    bool     is_read = false, answering = false;
    int      reg_addr = 0;

    int      frames = 0, writes = 0, reads = 0, short_preambles = 0;
    uint32_t last_word = 0;

    void drive(bool o, bool v) { pending.push_back({now + valid_delay, o, v}); }

    void step() {
        while (!pending.empty() && pending.front().at <= now) {
            oe = pending.front().oe;
            val = pending.front().val;
            pending.pop_front();
        }
    }

    // MDC rose with `level` on the pin, the master driving it or not.
    void rise(bool level, bool master_drives) {
        if (!in_frame) {
            if (level) {
                ones++;
                driven_ones = master_drives ? driven_ones + 1 : 0;
                return;
            }
            if (ones >= 32) {
                check(driven_ones >= 32, "the master drove %d of the preamble's ones",
                      driven_ones);
                in_frame = true;
                nbit = 1;                   // this zero opens the start pattern
                word = 0;
                is_read = answering = false;
            } else {
                short_preambles++;
            }
            ones = driven_ones = 0;
            return;
        }

        word = (word << 1) | (level ? 1u : 0u);
        nbit++;

        if (nbit == 14) {                   // start, operation and both addresses
            bool start_ok = ((word >> 12) & 3) == 1;
            int  op  = (word >> 10) & 3;
            int  phy_addr = (word >> 5) & 31;
            reg_addr = word & 31;
            check(start_ok, "start pattern %x", (word >> 12) & 3);
            check(op == 1 || op == 2, "operation %x", op);
            is_read = (op == 2);
            answering = start_ok && is_read && phy_addr == PHY_ADDR;
        }

        if (nbit > 14) {
            // Turnaround and data. A read's are the PHY's to drive: the
            // master is off the pin for all of them, answered or not.
            if (is_read)
                check(!master_drives, "the master drives bit %d of a read's word",
                      nbit - 1);
            else
                check(master_drives, "the master lets go of bit %d of a write's word",
                      nbit - 1);
        }

        if (answering) {
            // A bit goes out on the rise before the one it is taken on:
            // the turnaround's zero first, then the data from the top.
            if (nbit == 15)
                drive(true, false);
            else if (nbit >= 16 && nbit <= 31)
                drive(true, (regs[reg_addr] >> (31 - nbit)) & 1);
            else if (nbit == 32)
                drive(false, true);         // let go on the last bit's rise
        }

        if (nbit == 32) {
            frames++;
            last_word = word;
            if (is_read) {
                if (answering) reads++;
            } else {
                check(((word >> 16) & 3) == 2, "a write's turnaround %x",
                      (word >> 16) & 3);
                if (((word >> 23) & 31) == PHY_ADDR) {
                    regs[(word >> 18) & 31] = word & 0xffff;
                    writes++;
                }
            }
            in_frame = false;
            ones = driven_ones = 0;
        }
    }
} phy;

// ── The monitor ──────────────────────────────────────────────
struct Monitor {
    bool prev_mdc = false;
    int  prev_master = 2;               // 0, 1, or 2 for off the pin
    long last_rise = -1000, last_fall = -1000, last_master_change = -1000;
    int  rises = 0;

    void step(bool mdc, bool master_oe, bool master_val, bool pin) {
        int master = master_oe ? (master_val ? 1 : 0) : 2;
        if (master != prev_master) {
            check(now - last_rise >= cycles(T_HOLD),
                  "data pin moved %ld cycles after MDC rose", now - last_rise);
            last_master_change = now;
            prev_master = master;
        }
        if (mdc && !prev_mdc) {
            check(now - last_fall >= cycles(T_MDC_LOW), "MDC low for %ld cycles",
                  now - last_fall);
            check(now - last_rise >= cycles(T_MDC_PERIOD), "MDC period %ld cycles",
                  now - last_rise);
            check(now - last_master_change >= cycles(T_SETUP),
                  "data pin moved %ld cycles before MDC rose",
                  now - last_master_change);
            rises++;
            phy.rise(pin, master_oe);
            last_rise = now;
        }
        if (!mdc && prev_mdc) {
            check(now - last_rise >= cycles(T_MDC_HIGH), "MDC high for %ld cycles",
                  now - last_rise);
            last_fall = now;
        }
        prev_mdc = mdc;
    }
} mon;

// One clock cycle, then the bus as the pins and the PHY leave it.
void tick() {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
    now++;

    phy.step();
    bool master_oe = dut->o_mdio_oe, master_val = dut->o_mdio;
    check(!(master_oe && phy.oe), "master and PHY both drive the data pin at %ld", now);
    bool pin = master_oe ? master_val : phy.oe ? phy.val : true;
    mon.step(dut->o_mdc, master_oe, master_val, pin);
    dut->i_mdio = pin;
}

void idle(int n) {
    for (int i = 0; i < n; i++) {
        tick();
        check(!dut->o_busy && !dut->o_done && !dut->o_mdc && !dut->o_mdio_oe,
              "not at rest while idle, at %ld", now);
    }
}

// Start one frame and run it to its end. Returns in the cycle o_done is
// up, so that a caller can start the next one at the first chance.
uint16_t frame(bool write, int phy_addr, int reg_addr, uint16_t data) {
    int rises_before = mon.rises, frames_before = phy.frames;

    dut->i_start = 1;
    dut->i_write = write;
    dut->i_phy_addr = phy_addr;
    dut->i_reg_addr = reg_addr;
    dut->i_wdata = data;
    tick();
    check(dut->o_busy, "busy the cycle after a start");
    check(!dut->o_done, "done lasts past its cycle");

    // The frame is what the start said, whatever the inputs do after.
    dut->i_start = 0;
    dut->i_write = !write;
    dut->i_phy_addr = ~phy_addr & 31;
    dut->i_reg_addr = ~reg_addr & 31;
    dut->i_wdata = (uint16_t)~data;

    long t0 = now;
    int  dones = 0;
    while (dut->o_busy && now - t0 < 2 * cycles(T_BUSY_NS)) {
        tick();
        if (dut->o_done) dones++;
    }
    check(!dut->o_busy, "still busy after %ld cycles", now - t0);
    check((now - t0) <= cycles(T_BUSY_NS), "busy for %ld cycles", now - t0);
    check(dones == 1 && dut->o_done, "done pulsed %d times", dones);
    check(mon.rises - rises_before == FRAME_BITS, "MDC rose %d times in a frame",
          mon.rises - rises_before);
    check(phy.frames - frames_before == 1, "the PHY saw %d frames",
          phy.frames - frames_before);
    return dut->o_rdata;
}

} // namespace

int main() {
    dut = new Veth_mdio_test;
    dut->i_clk = 0;
    dut->i_start = 0;
    dut->i_write = 0;
    dut->i_phy_addr = 0;
    dut->i_reg_addr = 0;
    dut->i_wdata = 0;
    dut->i_mdio = 1;

    // As configured, with no reset to ask for it: at rest.
    idle(200);
    check(mon.rises == 0, "MDC ran before any start");

    // A write reaches the PHY whole.
    frame(true, PHY_ADDR, 4, 0x01e1);
    check(phy.last_word == frame_word(true, PHY_ADDR, 4, 0x01e1),
          "write frame word %08x", phy.last_word);
    check(phy.regs[4] == 0x01e1 && phy.writes == 1, "the PHY's register %04x",
          phy.regs[4]);
    idle(50);

    // One addressed elsewhere is sent as whole and changes nothing here.
    frame(true, 7, 4, 0xffff);
    check(phy.last_word == frame_word(true, 7, 4, 0xffff), "write elsewhere %08x",
          phy.last_word);
    check(phy.regs[4] == 0x01e1 && phy.writes == 1, "a write elsewhere landed here");
    idle(50);

    // A read comes back whole, the PHY answering at once and at the end
    // of its allowance.
    phy.regs[1] = 0x796d;
    phy.regs[2] = 0x5045;
    for (int delay : {0, (int)(T_VALID / NS_PER_CYCLE)}) {
        phy.valid_delay = delay;
        uint16_t r1 = frame(false, PHY_ADDR, 1, 0xbeef);
        check(r1 == 0x796d, "read with the PHY %d cycles late: %04x", delay, r1);
        check((phy.last_word >> 18) == (frame_word(false, PHY_ADDR, 1, 0) >> 18),
              "read frame word %08x", phy.last_word);

        // The next frame started at the first chance finds the pin free.
        uint16_t r2 = frame(false, PHY_ADDR, 2, 0);
        check(r2 == 0x5045, "second read, started at once: %04x", r2);
        uint16_t r3 = frame(true, PHY_ADDR, 9, 0x0200);
        (void)r3;
        check(phy.regs[9] == 0x0200, "write started at once after a read: %04x",
              phy.regs[9]);
        idle(50);
    }
    check(phy.reads == 4, "the PHY answered %d reads", phy.reads);

    // Nothing answers at another address, and the pull-up reads as ones.
    {
        uint16_t r = frame(false, 3, 1, 0x1234);
        check(r == 0xffff, "read where no PHY answers: %04x", r);
        idle(50);
    }

    // A start while busy is not a frame and does not disturb the one in
    // flight.
    {
        int frames_before = phy.frames;
        dut->i_start = 1;
        dut->i_write = 0;
        dut->i_phy_addr = PHY_ADDR;
        dut->i_reg_addr = 1;
        dut->i_wdata = 0;
        tick();
        dut->i_start = 0;
        for (int i = 0; i < 123; i++) tick();
        dut->i_start = 1;                   // mid-frame: a write, elsewhere
        dut->i_write = 1;
        dut->i_phy_addr = 9;
        dut->i_reg_addr = 9;
        dut->i_wdata = 0xdead;
        for (int i = 0; i < 3; i++) tick();
        dut->i_start = 0;
        while (dut->o_busy) tick();
        check(dut->o_rdata == 0x796d, "read with a start during it: %04x",
              dut->o_rdata);
        idle(2000);
        check(phy.frames - frames_before == 1, "a start while busy made %d frames",
              phy.frames - frames_before);
    }

    // A spread of reads and writes against the PHY's registers.
    {
        uint32_t lcg = 0x1234567u;
        auto next = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };
        for (int n = 0; n < 200; n++) {
            bool     write = next() & 1;
            int      addr  = (next() & 3) ? PHY_ADDR : (int)(next() & 31);
            int      reg   = next() & 31;
            uint16_t data  = (uint16_t)next();
            phy.valid_delay = next() % (T_VALID / NS_PER_CYCLE + 1);

            uint16_t before = phy.regs[reg];
            uint16_t got = frame(write, addr, reg, data);
            if (write)
                check(phy.regs[reg] == (addr == PHY_ADDR ? data : before),
                      "random write %d: register %04x", n, phy.regs[reg]);
            else
                check(got == (addr == PHY_ADDR ? before : 0xffff),
                      "random read %d: %04x", n, got);
            if (next() & 1) idle(next() % 40);
        }
    }

    check(phy.short_preambles == 0, "%d frames opened on a short preamble",
          phy.short_preambles);

    printf("eth_mdio: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

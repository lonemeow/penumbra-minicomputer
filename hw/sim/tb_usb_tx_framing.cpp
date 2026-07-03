// Verilator testbench for the Penumbra USB transmit framing FSM
//
// Models the serializer/stuffer handshakes in C++ (a bit budget for
// i_ser_active, a schedule of stuff bit-times for i_stuff, consumed on
// o_payload_en) and checks the packet phases bit-time by bit-time: eight
// SYNC bit times (o_sync_bit = 0..0,1) with the encoder init at start and the
// stuffer seed exactly on SYNC's last bit, payload bit times = data + stuff
// (including a trailing stuff after the last data bit), then SE0 for two bit
// times, J for one, and the drive released. Bit-time spacing is checked
// against the speed's divisor, and a second packet confirms re-arm.

#include <cstdio>
#include <cstdint>
#include <set>
#include <vector>
#include "Vusb_tx_framing.h"

enum { SPEED_FS = 0, SPEED_LS = 1 };
static int div_of(int speed) { return speed == SPEED_LS ? 40 : 5; }

static void settle(Vusb_tx_framing* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_tx_framing* d)   { d->i_clk = 1; d->eval(); }

struct Res {
    std::vector<int> sync_bits;       // o_sync_bit at SYNC bit times
    int nrzi_init = 0, stuff_init = 0;
    int payload_bit_times = 0;
    int consumed = 0;                 // data bits the model handed over
    int se0_bit_times = 0, j_bit_times = 0;
    bool oe_seen = false, oe_clean_idle = true;
    std::vector<long> tick_clocks;    // o_bit_en cycle numbers (spacing check)
    bool finished = false;            // returned to idle
};

// Run one packet: `payload_bits` data bits with stuff bit-times inserted after
// the counts in `stuff_after` (a count equal to payload_bits models the
// trailing stuff of a packet ending in six 1s).
static Res run_packet(Vusb_tx_framing* d, int speed, int payload_bits,
                      const std::set<int>& stuff_after, bool do_reset) {
    if (do_reset) {
        d->i_rst = 1; d->i_speed = speed;
        d->i_ser_active = 0; d->i_stuff = 0;
        settle(d); edge(d); settle(d); edge(d);
        d->i_rst = 0;
    }

    Res r;
    std::set<int> stuffed;
    long limit = (long)(payload_bits + 16) * div_of(speed) * 4 + 200;
    for (long c = 0; c < limit; c++) {
        bool stuff_due = stuff_after.count(r.consumed) && !stuffed.count(r.consumed);
        d->i_ser_active = (r.consumed < payload_bits);
        d->i_stuff      = stuff_due;

        settle(d);
        if (d->o_nrzi_init)  r.nrzi_init++;
        if (d->o_stuff_init) r.stuff_init++;
        if (d->o_bit_en) {
            r.tick_clocks.push_back(c);
            if (d->o_sync_sel) r.sync_bits.push_back(d->o_sync_bit);
            if (d->o_se0)      r.se0_bit_times++;
            if (d->o_drive_j)  r.j_bit_times++;
        }
        if (d->o_payload_en) {
            r.payload_bit_times++;
            if (stuff_due) stuffed.insert(r.consumed);
            else           r.consumed++;
        }
        if (d->o_oe) r.oe_seen = true;
        else if (r.oe_seen) { r.finished = true; break; }   // oe dropped: done
        edge(d);
    }
    return r;
}

static bool check(const char* name, const Res& r, int speed,
                  int payload_bits, size_t stuff_count) {
    const std::vector<int> sync_want = {0, 0, 0, 0, 0, 0, 0, 1};
    bool ok = r.finished
        && r.sync_bits == sync_want
        && r.nrzi_init == 1 && r.stuff_init == 1
        && r.payload_bit_times == payload_bits + (int)stuff_count
        && r.consumed == payload_bits
        && r.se0_bit_times == 2 && r.j_bit_times == 1;
    // Bit times must be exactly one divisor apart.
    for (size_t i = 1; ok && i < r.tick_clocks.size(); i++)
        if (r.tick_clocks[i] - r.tick_clocks[i - 1] != div_of(speed)) ok = false;
    if (!ok)
        printf("FAIL [%s]: fin=%d sync=%zu ninit=%d sinit=%d pay=%d/%d cons=%d/%d se0=%d j=%d\n",
               name, r.finished, r.sync_bits.size(), r.nrzi_init, r.stuff_init,
               r.payload_bit_times, payload_bits + (int)stuff_count,
               r.consumed, payload_bits, r.se0_bit_times, r.j_bit_times);
    return ok;
}

int main() {
    Vusb_tx_framing* dut = new Vusb_tx_framing;
    int pass = 0, fail = 0;

    struct Case { const char* name; int speed; int bits; std::set<int> stuff; };
    std::vector<Case> cases = {
        {"FS plain",          SPEED_FS, 16, {}},
        {"FS one stuff",      SPEED_FS, 16, {5}},
        {"FS start stuff",    SPEED_FS, 16, {0}},
        {"FS trailing stuff", SPEED_FS, 16, {16}},   // payload ends in six 1s
        {"FS dense stuff",    SPEED_FS, 24, {5, 11, 17, 24}},
        {"FS single bit",     SPEED_FS, 1,  {}},
        {"LS plain",          SPEED_LS, 8,  {}},
        {"LS trailing stuff", SPEED_LS, 8,  {8}},
    };
    for (const auto& c : cases) {
        Res r = run_packet(dut, c.speed, c.bits, c.stuff, true);
        (check(c.name, r, c.speed, c.bits, c.stuff.size()) ? pass : fail)++;
    }

    // Re-arm: a second packet on the same DUT with no reset in between.
    {
        Res r1 = run_packet(dut, SPEED_FS, 8, {}, true);
        Res r2 = run_packet(dut, SPEED_FS, 12, {12}, false);
        (check("b2b first", r1, SPEED_FS, 8, 0) ? pass : fail)++;
        (check("b2b second", r2, SPEED_FS, 12, 1) ? pass : fail)++;
    }

    printf("usb_tx_framing: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}

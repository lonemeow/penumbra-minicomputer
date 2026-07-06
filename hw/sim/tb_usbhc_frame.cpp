// Verilator testbench for the Penumbra USB host frame timer
//
// Drives usbhc_frame_test (200-clock frames) while emulating the MAC's
// transmit arbiter — grant immediately, grant late, or withhold across a
// whole frame — and the packet transmitter (done after a fixed delay).
// Checks frame counting and SOF pulses under RUN, marker transmission and
// its fields, keep-alive selection at low speed, the send-only-the-latest
// policy when a marker misses its frame, RUN gating, and the 11-bit wrap.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusbhc_frame_test.h"

static const int FRAME_CLKS = 200;   // matches the wrapper's CLKS_PER_MS
static const uint8_t PID_SOF = 0x5;
static const int SPEED_FS = 1, SPEED_LS = 2;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool ok) {
    if (ok) {
        g_pass++;
    } else {
        printf("MISMATCH %s\n", name);
        g_fail++;
    }
}

// Value form: one quantity per check, and the mismatch shows both sides.
static void check_eq(const char* name, long got, long want) {
    if (got == want) {
        g_pass++;
    } else {
        printf("MISMATCH %s: got %ld want %ld\n", name, got, want);
        g_fail++;
    }
}

struct Marker {
    uint16_t field;
    int keepalive;
};

// Cycle-level driver. Grant policy: grant_delay < 0 withholds the
// transmitter entirely; otherwise the grant rises grant_delay cycles after
// o_req and holds until o_req drops. tx_done pulses TX_CYCLES after each
// o_tx_start, mimicking a marker's transmit time.
struct Driver {
    Vusbhc_frame_test* dut;
    int grant_delay = 0;
    int grant_countdown = -1;
    int tx_countdown = -1;
    std::vector<Marker> markers;
    int sof_irqs = 0;
    bool bad_pid = false;

    static const int TX_CYCLES = 20;

    explicit Driver(Vusbhc_frame_test* d) : dut(d) {}

    void cycle() {
        dut->i_tx_done = 0;
        if (tx_countdown > 0 && --tx_countdown == 0)
            dut->i_tx_done = 1;

        if (dut->o_req) {
            if (grant_delay >= 0 && !dut->i_grant) {
                if (grant_countdown < 0)
                    grant_countdown = grant_delay;
                if (grant_countdown-- == 0)
                    dut->i_grant = 1;
            }
        } else {
            dut->i_grant = 0;
            grant_countdown = -1;
        }

        dut->i_clk = 0;
        dut->eval();
        if (dut->o_tx_start) {
            markers.push_back({(uint16_t)dut->o_tx_field,
                               (int)dut->o_tx_keepalive});
            if (dut->o_tx_pid != PID_SOF)
                bad_pid = true;
            tx_countdown = TX_CYCLES;
        }
        dut->i_clk = 1;
        dut->eval();
        if (dut->o_sof_irq)
            sof_irqs++;
    }

    void run(int cycles) {
        for (int i = 0; i < cycles; i++)
            cycle();
    }
};

int main() {
    Vusbhc_frame_test* dut = new Vusbhc_frame_test;

    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_run = 0;
    dut->i_speed = SPEED_FS;
    dut->i_grant = 0;
    dut->i_tx_done = 0;
    {
        Driver rst(dut);
        rst.run(2);
    }
    dut->i_rst = 0;

    // Stopped: no frames elapse, nothing is requested.
    {
        Driver d(dut);
        d.run(3 * FRAME_CLKS);
        check("stopped: no irq", d.sof_irqs == 0);
        check("stopped: no markers", d.markers.empty());
        check("stopped: frame frozen", dut->o_frame == 0);
    }

    // Running at full speed with an immediate grant: one SOF per frame,
    // consecutive frame numbers, no keep-alive flag.
    dut->i_run = 1;
    {
        Driver d(dut);
        d.run(5 * FRAME_CLKS + FRAME_CLKS / 2);
        check_eq("run: sof irqs", d.sof_irqs, 5);
        check_eq("run: markers sent", (long)d.markers.size(), 5);
        check_eq("run: frame counter", dut->o_frame, 5);
        bool fields_ok = true, ka_ok = true;
        for (size_t i = 0; i < d.markers.size(); i++) {
            fields_ok &= d.markers[i].field == i + 1;
            ka_ok &= d.markers[i].keepalive == 0;
        }
        check("run: SOF fields consecutive", fields_ok);
        check("run: no keep-alive at FS", ka_ok);
        check("run: SOF PID", !d.bad_pid);
    }

    // Low speed: the marker is the keep-alive.
    dut->i_speed = SPEED_LS;
    {
        Driver d(dut);
        d.run(2 * FRAME_CLKS);
        check_eq("LS: markers sent", (long)d.markers.size(), 2);
        check("LS: keep-alive flag",
              d.markers.size() == 2 && d.markers[0].keepalive == 1 &&
              d.markers[1].keepalive == 1);
    }
    dut->i_speed = SPEED_FS;

    // A late grant (a transaction holds the transmitter briefly): the
    // marker still goes out, with its own frame's number.
    {
        Driver d(dut);
        d.grant_delay = FRAME_CLKS / 4;
        uint16_t before = dut->o_frame;
        d.run(FRAME_CLKS + FRAME_CLKS / 2);
        check_eq("late grant: markers sent", (long)d.markers.size(), 1);
        if (d.markers.size() == 1)
            check_eq("late grant: marker field", d.markers[0].field,
                     (before + 1) & 0x7ff);
        else
            check("late grant: marker field", false);
    }

    // A marker that misses its whole frame: the counter keeps elapsing,
    // and only the latest marker is sent once the transmitter frees.
    {
        Driver d(dut);
        d.grant_delay = -1;                     // withhold
        uint16_t before = dut->o_frame;
        d.run(2 * FRAME_CLKS + FRAME_CLKS / 4); // two boundaries pass
        check_eq("missed frame: counter elapsed", dut->o_frame,
                 (before + 2) & 0x7ff);
        check_eq("missed frame: nothing sent yet", (long)d.markers.size(), 0);
        d.grant_delay = 0;                      // release the transmitter
        d.run(FRAME_CLKS / 4);
        check_eq("missed frame: exactly one marker", (long)d.markers.size(), 1);
        if (d.markers.size() == 1)
            check_eq("missed frame: latest frame number", d.markers[0].field,
                     (before + 2) & 0x7ff);
        else
            check("missed frame: latest frame number", false);
    }

    // Dropping RUN clears a due marker instead of sending it later.
    {
        Driver d(dut);
        d.grant_delay = -1;
        d.run(FRAME_CLKS + FRAME_CLKS / 4);     // a marker becomes due
        dut->i_run = 0;
        d.grant_delay = 0;
        d.run(FRAME_CLKS);
        check("run dropped: due marker cleared",
              d.markers.empty() && dut->o_req == 0);
        dut->i_run = 1;
    }

    // The 11-bit frame counter wraps cleanly through 2047 -> 0.
    {
        Driver d(dut);
        uint16_t start = dut->o_frame;
        int frames = 2050;
        d.run(frames * FRAME_CLKS + FRAME_CLKS / 2);
        check_eq("wrap: counter", dut->o_frame, (start + frames) & 0x7ff);
        bool fields_ok = (int)d.markers.size() == frames;
        for (size_t i = 0; i < d.markers.size() && fields_ok; i++)
            fields_ok = d.markers[i].field == ((start + 1 + i) & 0x7ff);
        check("wrap: every SOF field", fields_ok);
    }

    printf("usbhc_frame: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

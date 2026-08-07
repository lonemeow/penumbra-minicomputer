// Verilator testbench for the Penumbra USB PHY pair
//
// Drives host A's seam like the MAC does and checks what arrives — as
// bytes at peer B's seam (packet transport through both composed chains
// and the wire), and as cycle-counted waveforms on the resolved bus (the
// out-of-band recipes: bus reset SE0, resume K with its appended EOP, the
// low-speed keep-alive's bare EOP). Also pins the composition-only
// behaviors: the receiver never hears its own transmissions, the
// non-driving opmode releases the line mid-packet, and the line-state
// sideband filters transition-skew SE0 glitches but reports real SE0.
//
// Byte content is opaque at this layer (CRC lives in the MAC), so packets
// are arbitrary — including all-ones payloads that force bit stuffing
// across the pair.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_phy_pair_test.h"

static const int SPEED_FS = 1, SPEED_LS = 2;
static const int OPMODE_NORMAL = 0, OPMODE_NONDRIVE = 1, OPMODE_RAW = 2;

static Vusb_phy_pair_test* dut;
static int g_pass = 0, g_fail = 0;

static void expect(const char* name, bool ok) {
    if (ok) {
        g_pass++;
        return;
    }
    printf("FAIL %s\n", name);
    g_fail++;
}

static int clocks_per_bit(int speed) { return speed == SPEED_LS ? 40 : 5; }

// ── Receive collectors, polled every tick ────────────────────────────────

struct Collector {
    std::vector<uint8_t> bytes;
    bool active_seen = false;
    bool complete = false;
    bool error = false;
    void reset() { bytes.clear(); active_seen = complete = error = false; }
};
static Collector col_b;
static bool a_heard_itself;   // A's receive window opened during A's own TX

static void poll() {
    if (dut->o_b_rx_valid)
        col_b.bytes.push_back(dut->o_b_rx_data);
    if (dut->o_b_rx_active)
        col_b.active_seen = true;
    else if (col_b.active_seen)
        col_b.complete = true;
    if (dut->o_b_rx_error)
        col_b.error = true;
    if (dut->o_a_rx_active || dut->o_a_rx_valid)
        a_heard_itself = true;
}

// Scheduled one-clock SE0 glitch: models the false SE0 that D+/D-
// edge skew produces at a symbol transition when the lines are sensed
// single-ended.  Armed by setting a countdown in ticks; fires as a
// one-tick bus override, then releases.
static int g_glitch_in = -1;

static void tick() {
    bool glitch = (g_glitch_in == 0);
    if (g_glitch_in >= 0)
        g_glitch_in--;
    if (glitch) {
        dut->i_force_en = 1;
        dut->i_force_dp = 0;
        dut->i_force_dn = 0;
    }
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
    poll();
    if (glitch)
        dut->i_force_en = 0;
}

static void run(int cycles) {
    for (int i = 0; i < cycles; i++)
        tick();
}

// ── Seam driving ─────────────────────────────────────────────────────────

// Presents a packet at host A's transmit seam; returns bytes consumed.
static int send_a(const std::vector<uint8_t>& bytes, int cap) {
    size_t idx = 0;
    dut->i_a_tx_data = bytes[0];
    dut->i_a_tx_valid = 1;
    for (int c = 0; c < cap && idx < bytes.size(); c++) {
        tick();
        if (dut->o_a_tx_ready) {
            idx++;
            if (idx < bytes.size())
                dut->i_a_tx_data = bytes[idx];
        }
    }
    dut->i_a_tx_valid = 0;
    return (int)idx;
}

static int send_b(const std::vector<uint8_t>& bytes, int cap) {
    size_t idx = 0;
    dut->i_b_tx_data = bytes[0];
    dut->i_b_tx_valid = 1;
    for (int c = 0; c < cap && idx < bytes.size(); c++) {
        tick();
        if (dut->o_b_tx_ready) {
            idx++;
            if (idx < bytes.size())
                dut->i_b_tx_data = bytes[idx];
        }
    }
    dut->i_b_tx_valid = 0;
    return (int)idx;
}

// Runs until B's collector closes a packet (or the cap).
static void drain_to_b(int cap) {
    for (int c = 0; c < cap && !col_b.complete; c++)
        tick();
}

// ── Waveform measurement ─────────────────────────────────────────────────

// Cycle counts of each bus state over one drive burst of host A: waits for
// o_a_oe to rise, classifies every driven cycle, returns at release.
struct Burst {
    int se0 = 0, j = 0, k = 0, total = 0;
    bool seen = false;
};

static Burst measure_a_burst(int speed, int cap) {
    Burst b;
    int c = 0;
    while (c++ < cap && !dut->o_a_oe)
        tick();
    if (!dut->o_a_oe)
        return b;
    b.seen = true;
    bool idle_dp = (speed != SPEED_LS);
    while (dut->o_a_oe && c++ < cap) {
        if (!dut->o_bus_dp && !dut->o_bus_dn)
            b.se0++;
        else if (dut->o_bus_dp == idle_dp)
            b.j++;
        else
            b.k++;
        b.total++;
        tick();
    }
    return b;
}

static bool near(int got, int want, int tol) {
    return got >= want - tol && got <= want + tol;
}

// ── Scenario helpers ─────────────────────────────────────────────────────

static void set_speed(int speed) {
    dut->i_speed = speed;
    dut->i_a_xcvr_sel = speed;
    dut->i_a_term_sel = 1;
    dut->i_a_opmode = OPMODE_NORMAL;
}

static void packet_roundtrip(const char* name, int speed,
                             const std::vector<uint8_t>& payload) {
    set_speed(speed);
    run(20 * clocks_per_bit(speed));   // settle at idle
    col_b.reset();
    a_heard_itself = false;
    int cap = (int)(payload.size() + 16) * 8 * clocks_per_bit(speed) * 4;
    int consumed = send_a(payload, cap);
    drain_to_b(cap);
    char tag[64];
    snprintf(tag, sizeof(tag), "%s transport", name);
    expect(tag, consumed == (int)payload.size() && col_b.complete &&
                col_b.bytes == payload && !col_b.error);
    snprintf(tag, sizeof(tag), "%s squelch", name);
    expect(tag, !a_heard_itself);
}

int main() {
    dut = new Vusb_phy_pair_test;
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_speed = SPEED_FS;
    dut->i_force_en = 0;
    dut->i_force_dp = 0;
    dut->i_force_dn = 0;
    dut->i_a_tx_data = 0;
    dut->i_a_tx_valid = 0;
    dut->i_a_opmode = OPMODE_NORMAL;
    dut->i_a_xcvr_sel = SPEED_FS;
    dut->i_a_term_sel = 1;
    dut->i_b_tx_data = 0;
    dut->i_b_tx_valid = 0;
    tick();
    tick();
    dut->i_rst = 0;
    run(8);

    // ── Constants and pull enables ───────────────────────────────────
    expect("caps fs+ls", dut->o_a_caps == 0x3);
    expect("host pull-downs resident", dut->o_a_pull_dp && dut->o_a_pull_dn);

    // ── Packet transport, both speeds, both directions ───────────────
    packet_roundtrip("fs token-shaped", SPEED_FS, {0x2d, 0x00, 0x10});
    std::vector<uint8_t> data_img{0xc3, 0xde, 0xad, 0xbe, 0xef,
                                  0x11, 0x22, 0x33, 0x44, 0x64, 0x3e};
    packet_roundtrip("fs data-shaped", SPEED_FS, data_img);
    // All-ones payload forces a stuff bit every six line bits.
    packet_roundtrip("fs stuffing", SPEED_FS,
                     {0xff, 0xff, 0xff, 0xff, 0x7e});
    packet_roundtrip("ls token-shaped", SPEED_LS, {0xe1, 0x03, 0x28});

    // Peer to host: B transmits, A receives.
    set_speed(SPEED_FS);
    run(100);
    std::vector<uint8_t> reply{0x4b, 0xa5, 0x5a};
    {
        std::vector<uint8_t> got;
        bool a_active = false, a_done = false;
        size_t idx = 0;
        dut->i_b_tx_data = reply[0];
        dut->i_b_tx_valid = 1;
        for (int c = 0; c < 8000 && !a_done; c++) {
            tick();
            if (dut->o_b_tx_ready && idx < reply.size()) {
                idx++;
                if (idx < reply.size())
                    dut->i_b_tx_data = reply[idx];
                else
                    dut->i_b_tx_valid = 0;
            }
            if (dut->o_a_rx_valid)
                got.push_back(dut->o_a_rx_data);
            if (dut->o_a_rx_active)
                a_active = true;
            else if (a_active)
                a_done = true;
        }
        dut->i_b_tx_valid = 0;
        expect("b-to-a transport", a_done && got == reply);
    }

    // ── Low-speed keep-alive: lone A5h becomes a bare EOP ────────────
    set_speed(SPEED_LS);
    run(200);
    col_b.reset();
    {
        const int ls = clocks_per_bit(SPEED_LS);
        dut->i_a_tx_data = 0xa5;
        dut->i_a_tx_valid = 1;
        int c = 0;
        bool consumed = false;
        while (c++ < 20 && !consumed) {
            tick();
            consumed = dut->o_a_tx_ready;
        }
        dut->i_a_tx_valid = 0;
        expect("keep-alive byte consumed", consumed);
        Burst b = measure_a_burst(SPEED_LS, 20 * ls);
        // A bare EOP: two bit times of SE0 and one of J — no SYNC, no
        // payload (a serialized packet would run ~19 bit times).
        expect("keep-alive wave shape", b.seen &&
               near(b.se0, 2 * ls, 4) && near(b.j, ls, 4) && b.k == 0);
        run(4 * ls);
        expect("keep-alive silent at the peer",
               !col_b.active_seen && col_b.bytes.empty());
    }

    // ── Resume: raw K while held, the LS EOP appended on release ─────
    {
        const int ls = clocks_per_bit(SPEED_LS);
        dut->i_a_opmode = OPMODE_RAW;
        dut->i_a_tx_data = 0x00;
        dut->i_a_tx_valid = 1;
        run(4);
        expect("resume drives K", dut->o_a_oe &&
               !(dut->o_bus_dp == 0 && dut->o_bus_dn == 0) &&
               dut->o_bus_dp != (dut->i_speed != SPEED_LS));
        run(20 * ls);   // the software-timed hold, scaled down
        dut->i_a_tx_valid = 0;
        dut->i_a_opmode = OPMODE_NORMAL;
        Burst b = measure_a_burst(SPEED_LS, 30 * ls);
        expect("resume EOP tail", b.seen &&
               near(b.se0, 2 * ls, 4) && near(b.j, ls, 4));
        expect("resume releases", !dut->o_a_oe);
    }

    // ── Bus reset: the HS-termination state drives SE0 ───────────────
    {
        dut->i_a_xcvr_sel = 0;      // USB_SPEED_HS
        dut->i_a_term_sel = 0;
        dut->i_a_opmode = OPMODE_RAW;
        run(8);
        expect("reset drives SE0", dut->o_a_oe &&
               !dut->o_bus_dp && !dut->o_bus_dn);
        run(400);
        expect("reset holds SE0", dut->o_a_oe &&
               !dut->o_bus_dp && !dut->o_bus_dn);
        set_speed(SPEED_LS);
        run(8);
        expect("reset releases", !dut->o_a_oe);
    }

    // ── Non-driving opmode releases the line mid-packet ──────────────
    {
        const int ls = clocks_per_bit(SPEED_LS);
        set_speed(SPEED_LS);
        run(20 * ls);
        col_b.reset();
        dut->i_a_tx_data = 0x0f;
        dut->i_a_tx_valid = 1;
        int c = 0;
        while (c++ < 20 * ls && !dut->o_a_oe)
            tick();
        run(6 * ls);                 // into the packet body
        dut->i_a_opmode = OPMODE_NONDRIVE;
        run(4);
        expect("non-driving releases mid-packet", !dut->o_a_oe);
        dut->i_a_tx_valid = 0;
        run(40 * ls);                // let the chain drain, gated
        dut->i_a_opmode = OPMODE_NORMAL;
        // The peer saw a packet truncated to idle J: a stuff violation,
        // recovered by a forced EOP from the wire.
        expect("peer flags the truncation", col_b.error);
        dut->i_force_en = 1;
        dut->i_force_dp = 0;
        dut->i_force_dn = 0;
        run(3 * ls);
        dut->i_force_en = 0;
        run(4 * ls);
    }

    // ── Line-state sideband: glitch filter ───────────────────────────
    {
        set_speed(SPEED_FS);
        run(100);
        // Idle J at full speed reads raw {D-, D+} = 01.
        expect("line idles at J", dut->o_a_line_state == 0x1);

        // A one-cycle false SE0 (transition skew) must never be reported.
        bool saw_se0 = false;
        dut->i_force_en = 1;
        dut->i_force_dp = 0;
        dut->i_force_dn = 0;
        tick();
        dut->i_force_dp = 1;
        for (int c = 0; c < 12; c++) {
            tick();
            saw_se0 |= (dut->o_a_line_state == 0x0);
        }
        expect("skew glitch filtered", !saw_se0);

        // A held SE0 is reported once the window passes.
        dut->i_force_dp = 0;
        int c = 0;
        while (c++ < 12 && dut->o_a_line_state != 0x0)
            tick();
        expect("held SE0 reported", dut->o_a_line_state == 0x0);
        dut->i_force_en = 0;
        run(20);
        expect("line returns to J", dut->o_a_line_state == 0x1);

        // The window scales with the bit time at low speed.
        set_speed(SPEED_LS);
        run(200);
        expect("ls idle polarity", dut->o_a_line_state == 0x2);
        saw_se0 = false;
        dut->i_force_en = 1;
        dut->i_force_dp = 0;
        dut->i_force_dn = 0;
        for (int c2 = 0; c2 < 10; c2++) {
            tick();
            saw_se0 |= (dut->o_a_line_state == 0x0);
        }
        expect("ls short SE0 filtered", !saw_se0);
        run(20);
        expect("ls held SE0 reported", dut->o_a_line_state == 0x0);
        dut->i_force_en = 0;
        run(200);
    }

    // ── Skew-glitch immunity: one-clock false SE0s mid-packet ────────
    // The receive tap reads the SE0-filtered line, so a packet crossing
    // a skew glitch must arrive intact instead of truncating at a
    // false EOP.
    //
    // The first iteration also crosses a speed switch: set_speed flips
    // LS -> FS over an idle-J line, which reads the stale filtered
    // polarity as K for one cycle — a spurious SOP with no packet behind
    // it. The framing must shrug it off (its no-SYNC-zero abort) or the
    // first packet arrives with its SYNC routed into the byte stream.
    for (int off_bits : {12, 25, 48}) {
        set_speed(SPEED_FS);
        run(100);
        col_b.reset();
        a_heard_itself = false;
        std::vector<uint8_t> pl{0xC3, 0x80, 0x06, 0x00, 0x01, 0xFF, 0x40, 0x5A};
        int cap = (int)(pl.size() + 16) * 8 * clocks_per_bit(SPEED_FS) * 4;
        g_glitch_in = off_bits * clocks_per_bit(SPEED_FS);
        int consumed = send_a(pl, cap);
        drain_to_b(cap);
        g_glitch_in = -1;
        char tag[64];
        snprintf(tag, sizeof(tag), "skew glitch at bit %d", off_bits);
        bool ok = consumed == (int)pl.size() && col_b.complete &&
                  col_b.bytes == pl && !col_b.error;
        if (!ok) {
            printf("  dbg[%s]: consumed=%d complete=%d bytes=%zu/%zu err=%d\n  got:",
                   tag, consumed, (int)col_b.complete, col_b.bytes.size(),
                   pl.size(), (int)col_b.error);
            for (uint8_t b : col_b.bytes) printf(" %02x", b);
            printf("\n  exp:");
            for (uint8_t b : pl) printf(" %02x", b);
            printf("\n");
        }
        expect(tag, ok);
    }

    // ── Wire-impairment injection: per-edge SE0 slivers, dribble ─────
    // Hand-built packet waveforms driven onto the bus through the force
    // override while both PHYs idle; B receives through its SE0 filter.
    // Models what a real line does that the PHY-to-PHY path cannot:
    // D+/D- skew reading as a one-clock SE0 at every symbol transition,
    // and a dribble bit — up to one extra bit time the transmitter may
    // append before EOP, which a receiver must tolerate (a stuff
    // violation manufactured by dribble on a legal five-ones tail must
    // not reject the packet).
    {
        struct Pin { int dp, dn; };
        const Pin J{1, 0}, K{0, 1}, SE0{0, 0};
        const int div = clocks_per_bit(SPEED_FS);

        // SYNC + stuffed payload as NRZI symbols (FS polarity).
        auto symbols_of = [&](const std::vector<uint8_t>& payload) {
            std::vector<int> bits(7, 0);
            bits.push_back(1);
            int ones = 1;
            for (uint8_t b : payload)
                for (int i = 0; i < 8; i++) {
                    int bit = (b >> i) & 1;
                    bits.push_back(bit);
                    if (bit) { if (++ones == 6) { bits.push_back(0); ones = 0; } }
                    else ones = 0;
                }
            std::vector<Pin> sym;
            int level = 1;                  // idle J
            for (int b : bits) {
                if (!b) level ^= 1;
                sym.push_back(level ? J : K);
            }
            return sym;
        };

        // Expand symbols to clocks. skew: replace the first clock of
        // every symbol that changes the pin pair with SE0 (persistent
        // worst-case transition skew). dribble: hold the final level
        // one extra bit time before EOP.
        auto wave_of = [&](const std::vector<Pin>& sym, bool skew,
                           bool dribble) {
            std::vector<Pin> w(2 * div, J);
            Pin prev = J;
            for (const Pin& s : sym) {
                for (int t = 0; t < div; t++) {
                    bool edge0 = (t == 0) &&
                                 (s.dp != prev.dp || s.dn != prev.dn);
                    w.push_back((skew && edge0) ? SE0 : s);
                }
                prev = s;
            }
            if (dribble)
                for (int t = 0; t < div; t++) w.push_back(prev);
            for (int t = 0; t < 2 * div; t++) w.push_back(SE0);
            for (int t = 0; t < 4 * div; t++) w.push_back(J);
            return w;
        };

        auto inject = [&](const char* tag, const std::vector<uint8_t>& pl,
                          bool skew, bool dribble) {
            set_speed(SPEED_FS);
            run(40);
            col_b.reset();
            for (const Pin& p : wave_of(symbols_of(pl), skew, dribble)) {
                dut->i_force_en = 1;
                dut->i_force_dp = p.dp;
                dut->i_force_dn = p.dn;
                tick();
            }
            dut->i_force_en = 0;
            run(40);
            bool ok = col_b.complete && col_b.bytes == pl && !col_b.error;
            if (!ok)
                printf("  dbg[%s]: complete=%d bytes=%zu/%zu err=%d\n",
                       tag, (int)col_b.complete, col_b.bytes.size(),
                       pl.size(), (int)col_b.error);
            expect(tag, ok);
        };

        // Long pseudo-random payload — many transitions for the skew
        // cases, data-packet length scale.
        std::vector<uint8_t> data64;
        uint32_t g = 0x1234abcdu;
        for (int i = 0; i < 64; i++) {
            g = g * 1664525u + 1013904223u;
            data64.push_back((uint8_t)(g >> 24));
        }
        // 0xF8 ends the stuffed stream with a five-ones run (LSB-first:
        // 000 then five 1s) — the legal tail that phantom trailing 1s
        // (dribble, EOP-filter delay) would push over the stuff limit.
        std::vector<uint8_t> tail_ones{0x0F, 0xF8};

        inject("skew clean short", {0xC3, 0x5A}, true, false);
        inject("skew clean long", data64, true, false);
        inject("skew stuff-heavy", {0xFF, 0xFF, 0xFF, 0x7E}, true, false);
        inject("dribble short", {0xC3, 0x5A}, false, true);
        inject("dribble long", data64, false, true);
        inject("dribble five-ones tail", tail_ones, false, true);
        inject("skew+dribble long", data64, true, true);
        inject("skew+dribble five-ones tail", tail_ones, true, true);
        // No impairment: the injection path itself must be sound.
        inject("inject baseline", data64, false, false);
    }

    printf("usb_phy_pair: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

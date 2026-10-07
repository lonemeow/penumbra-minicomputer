// Verilator testbench for the RGMII PHY adapter
//
// A PHY on the adapter's pins and the Ethernet device at its seam, each
// holding the adapter to what it relies on. The two directions share
// nothing, and run side by side on unrelated clocks.
//
// A packet is a preamble, the start delimiter and then the frame. Data
// valid and transmit enable mark its nibbles, and are clear between
// packets.
//
//  * Receive: the PHY model puts nibbles on the pins as an RGMII PHY
//    does at 10 and 100 Mbit/s, packets with preambles of every length
//    from one nibble up, odd and even. At the seam, a packet is its start
//    delimiter and then its frame's bytes, two cycles apart, with nothing
//    presented ahead of the delimiter and a byte time with data valid
//    clear behind the last byte. An error the PHY reports on either
//    nibble is reported with that byte; a nibble left over at the end is
//    dropped; a burst with no delimiter in it presents nothing. Whatever
//    comes ahead of the delimiter, a clean preamble or not, is passed
//    over.
//  * Transmit: a model of the transmitter puts up a byte time and moves
//    on when it is taken. On the pins, the PHY model reads the control
//    pin as a PHY does, on the pin clock's rise and on the fall after it,
//    and must find every packet whole, low nibble first, with no error
//    and nothing between packets. The pin clock is square at a quarter of
//    the nibble rate's period per quarter, and no pin changes within a
//    quarter period of either of its edges, at both speeds and across a
//    change of speed.
//
// A change of speed may lose the frame in transit (the interface says
// so), so the transmit checks stand down for a few byte times around one
// and no packet is sent across it.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>
#include "Veth_phy_rgmii_xc7_test.h"

namespace {

Veth_phy_rgmii_xc7_test* dut;
int pass = 0, fail = 0;
long now = 0;                       // ps

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL [t=%ld ps: ", now); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 200) {
        printf("eth_phy_rgmii_xc7: gave up after %d failures\n", fail);
        exit(1);
    }
}

uint32_t lcg = 0x2468aceu;
uint32_t next_random() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }
int random_in(int lo, int hi) { return lo + (int)(next_random() % (uint32_t)(hi - lo + 1)); }

// ═════════════════════════════════════════════════════════════
//  Receive: what the PHY puts on the pins, and what the seam owes
// ═════════════════════════════════════════════════════════════

// One cycle of the receive clock on the pins.
struct RxCycle {
    uint8_t nibble;
    bool    dv;         // RX_DV: the nibble is a packet's
    bool    error;      // RX_ER
};

struct SeamByte {
    uint8_t byte;
    bool    error;
};

// A packet the seam owes: one whose delimiter was on the pins.
struct RxPacket {
    long                  delimiter_cycle;  // the pin cycle of the delimiter's second nibble
    std::vector<SeamByte> bytes;            // its frame: what follows the delimiter
    std::string           what;             // for a failure's message
    bool                  seen = false;
};

std::vector<RxCycle> rx_script;
std::vector<RxPacket> rx_packets;
// For a failure's message: which burst the pins held at each cycle.
std::vector<std::pair<long, std::string>> rx_bursts;    // (first cycle, description)

void rx_idle(int cycles, bool false_carrier = false) {
    for (int i = 0; i < cycles; i++) {
        // Between packets the data pins carry the link's status, or one
        // of the codes a PHY marks with the error half alone.
        RxCycle c;
        c.nibble = (uint8_t)random_in(0, 15);
        c.dv     = false;
        c.error  = false_carrier;
        rx_script.push_back(c);
    }
}

// A packet as the PHY delivers it: what comes ahead of the delimiter,
// the delimiter's D, the frame low nibble first, and perhaps one nibble
// too many. error_at is a nibble of the frame the PHY reports an error
// on, or -1.
void rx_packet_behind(const std::vector<RxCycle>& lead, const char* lead_what,
                      const std::vector<uint8_t>& frame, int error_at, bool dangling) {
    char what[200];
    snprintf(what, sizeof what, "%s, %zu bytes%s%s", lead_what, frame.size(),
             error_at >= 0 ? ", an error reported" : "",
             dangling ? ", a nibble left over" : "");
    rx_bursts.push_back({(long)rx_script.size(), what});

    for (const RxCycle& c : lead)
        rx_script.push_back(c);

    RxPacket f;
    f.delimiter_cycle = (long)rx_script.size();
    f.what = what;
    rx_script.push_back({0xD, true, false});

    for (size_t i = 0; i < frame.size(); i++) {
        bool lo_err = (error_at == (int)(2 * i));
        bool hi_err = (error_at == (int)(2 * i + 1));
        rx_script.push_back({(uint8_t)(frame[i] & 0xf), true, lo_err});
        rx_script.push_back({(uint8_t)(frame[i] >> 4), true, hi_err});
        f.bytes.push_back({frame[i], lo_err || hi_err});
    }
    if (dangling)
        rx_script.push_back({(uint8_t)random_in(0, 15), true, false});

    rx_packets.push_back(f);
}

// One behind a clean preamble.
void rx_packet(int preamble_nibbles, const std::vector<uint8_t>& frame,
               int error_at = -1, bool dangling = false) {
    char lead_what[60];
    snprintf(lead_what, sizeof lead_what, "a preamble of %d nibbles", preamble_nibbles);
    rx_packet_behind(std::vector<RxCycle>(preamble_nibbles, RxCycle{0x5, true, false}),
                     lead_what, frame, error_at, dangling);
}

// One behind nibbles of the caller's choosing, the last of them the 5 of
// the delimiter and none of the rest a delimiter.
void rx_packet_rough(const std::vector<uint8_t>& lead, const char* lead_what,
                     const std::vector<uint8_t>& frame) {
    std::vector<RxCycle> cycles;
    for (uint8_t n : lead)
        cycles.push_back({n, true, false});
    rx_packet_behind(cycles, lead_what, frame, -1, false);
}

// A burst with no delimiter in it.
void rx_burst(const std::vector<uint8_t>& nibbles, const char* what) {
    rx_bursts.push_back({(long)rx_script.size(), what});
    for (uint8_t n : nibbles)
        rx_script.push_back({n, true, false});
}

std::vector<uint8_t> random_bytes(int bytes) {
    std::vector<uint8_t> p;
    for (int i = 0; i < bytes; i++)
        p.push_back((uint8_t)next_random());
    return p;
}

const char* burst_at(long cycle) {
    const char* what = "no packet";
    for (auto& b : rx_bursts) {
        if (b.first > cycle) break;
        what = b.second.c_str();
    }
    return what;
}

void build_rx_script() {
    rx_idle(8);

    // Every preamble length from the delimiter alone up, odd and even.
    for (int p = 1; p <= 18; p++) {
        rx_packet(p, random_bytes(random_in(1, 20)));
        rx_idle(random_in(1, 30));
    }

    // Bytes that look like preamble and delimiter, inside a frame.
    rx_packet(15, {0x55, 0xD5, 0x5D, 0xD5, 0x55, 0x5D, 0xDD, 0x55, 0xD5});
    rx_idle(5);
    rx_packet(14, {0xD5, 0xD5, 0x55, 0x55, 0x5D});
    rx_idle(5);

    // A delimiter and nothing behind it.
    rx_packet(15, {});
    rx_idle(4);

    // One cycle between packets.
    rx_packet(15, random_bytes(12));
    rx_idle(1);
    rx_packet(15, random_bytes(12));
    rx_idle(1);
    rx_packet(4, random_bytes(3));
    rx_idle(6);

    // An error on a low nibble, on a high one, on the first and the last.
    rx_packet(15, random_bytes(10), 6);
    rx_idle(4);
    rx_packet(15, random_bytes(10), 7);
    rx_idle(4);
    rx_packet(15, random_bytes(10), 0);
    rx_idle(4);
    rx_packet(15, random_bytes(10), 19);
    rx_idle(4);

    // A nibble left over.
    rx_packet(15, random_bytes(9), -1, true);
    rx_idle(4);
    rx_packet(6, random_bytes(1), -1, true);
    rx_idle(4);

    // Something other than a clean preamble ahead of the delimiter.
    rx_packet_rough({0xA, 0xA, 5, 5, 5}, "a rough start to the preamble", random_bytes(9));
    rx_idle(4);
    rx_packet_rough({5, 5, 0xA, 5, 5}, "a preamble with a bad nibble in it", random_bytes(9));
    rx_idle(4);
    rx_packet_rough({5, 5, 0xA, 5}, "a preamble with a bad nibble in it, an even one",
                    random_bytes(9));
    rx_idle(4);
    rx_packet_rough({0xD, 5}, "a D ahead of the delimiter", random_bytes(9));
    rx_idle(4);
    rx_packet_rough({0, 0xF, 3, 5}, "no preamble but the delimiter's 5", random_bytes(9));
    rx_idle(4);
    // An error the PHY reports in the preamble is the preamble's.
    rx_packet_behind({{5, true, false}, {5, true, true}, {5, true, false}, {5, true, false},
                      {5, true, false}},
                     "a preamble with an error reported in it", random_bytes(9), -1, false);
    rx_idle(4);

    // Bursts with no delimiter in them.
    rx_burst({5, 5, 5, 5, 5, 5, 5, 5, 5}, "a preamble and no delimiter");
    rx_idle(3);
    rx_burst({0xA, 0xA, 0xA, 0xA, 0xA, 0xA}, "nibbles that are no preamble");
    rx_idle(3);
    rx_burst({1, 2, 3, 4, 6, 7, 8, 9, 0xD, 0xD, 5, 5, 0xE}, "nibbles with no delimiter among them");
    rx_idle(3);

    // What a PHY marks with the error half alone, between packets.
    rx_idle(12, true);
    rx_packet(15, random_bytes(8));
    rx_idle(2);
    rx_idle(3, true);

    // And at random.
    for (int i = 0; i < 400; i++) {
        int  bytes    = random_in(0, 70);
        bool dangling = (random_in(0, 9) == 0);
        int  error_at = (bytes > 0 && random_in(0, 9) == 0) ? random_in(0, 2 * bytes - 1) : -1;
        rx_packet(random_in(1, 20), random_bytes(bytes), error_at, dangling);
        rx_idle(random_in(1, 40), random_in(0, 15) == 0);
    }
    rx_idle(8);
}

// The pins reach the seam through the input cells' register and the
// adapter's own: a delimiter's byte is presented this many cycles behind
// its second nibble, at the least and at the most.
const long SEAM_LAG_MIN = 1;
const long SEAM_LAG_MAX = 4;

long   rx_cycle = 0;                // the pin cycle the next rising edge begins
size_t rx_next = 0;                 // the first packet whose delimiter is still to come at the seam
int    rx_in = -1;                  // the packet being presented, or -1
size_t rx_at = 0;                   // how many of its bytes have been
long   rx_last_byte = 0;            // the cycle of the last byte presented
bool   rx_lost = false;             // a failure left the seam mid-packet: wait for the gap
int    rx_packets_ok = 0;

void rx_seam(long cycle) {
    // A delimiter that should have been presented by now.
    while (rx_next < rx_packets.size() &&
           cycle > rx_packets[rx_next].delimiter_cycle + SEAM_LAG_MAX) {
        check(false,
              "packet %zu (%s): its delimiter was on the pins at cycle %ld "
              "and has not been presented",
              rx_next, rx_packets[rx_next].what.c_str(), rx_packets[rx_next].delimiter_cycle);
        rx_next++;
    }

    if (!dut->o_rx_ce)
        return;

    if (!dut->o_rx_dv) {
        if (rx_in >= 0) {
            const RxPacket& f = rx_packets[rx_in];
            check(rx_at == f.bytes.size(), "packet %d (%s): ended after %zu of its %zu bytes",
                  rx_in, f.what.c_str(), rx_at, f.bytes.size());
            if (rx_at == f.bytes.size())
                rx_packets_ok++;
            rx_in = -1;
        }
        rx_lost = false;
        return;
    }

    if (rx_lost)
        return;

    if (rx_in < 0) {
        // The first byte time of a packet is its delimiter. Whether an
        // error is reported with it is nobody's to read: the device
        // takes a frame's errors from the frame's bytes.
        bool due = rx_next < rx_packets.size() &&
                   cycle >= rx_packets[rx_next].delimiter_cycle + SEAM_LAG_MIN;
        if (dut->o_rxd != 0xD5 || !due) {
            check(false, "byte %02x presented with no delimiter before it (on the pins: %s)",
                  dut->o_rxd, burst_at(cycle - SEAM_LAG_MIN));
            rx_lost = true;
            return;
        }
        pass++;
        rx_packets[rx_next].seen = true;
        rx_in = (int)rx_next++;
        rx_at = 0;
        rx_last_byte = cycle;
        return;
    }

    const RxPacket& f = rx_packets[rx_in];
    check(cycle - rx_last_byte == 2,
          "packet %d (%s): byte %zu presented %ld cycles behind the one before, not two",
          rx_in, f.what.c_str(), rx_at, cycle - rx_last_byte);
    rx_last_byte = cycle;
    if (rx_at >= f.bytes.size()) {
        check(false, "packet %d (%s): byte %02x presented past its %zu bytes",
              rx_in, f.what.c_str(), dut->o_rxd, f.bytes.size());
        rx_lost = true;
        rx_in = -1;
        return;
    }
    check(dut->o_rxd == f.bytes[rx_at].byte,
          "packet %d (%s): byte %zu presented as %02x, the pins held %02x",
          rx_in, f.what.c_str(), rx_at, dut->o_rxd, f.bytes[rx_at].byte);
    check((bool)dut->o_rx_er == f.bytes[rx_at].error,
          "packet %d (%s): byte %zu presented with error %d, the PHY reported %d",
          rx_in, f.what.c_str(), rx_at, dut->o_rx_er, f.bytes[rx_at].error);
    rx_at++;
}

bool rx_done() { return rx_cycle >= (long)rx_script.size() + SEAM_LAG_MAX + 2; }

// What the PHY holds on the pins this cycle: the script's, and nothing
// once that has run out.
RxCycle rx_now() {
    if (rx_cycle < (long)rx_script.size())
        return rx_script[rx_cycle];
    return RxCycle{0, false, false};
}

// The PHY changes its pins on its clock's edges, and the clock reaches
// the adapter's input cells behind them.
void rx_rise() {
    RxCycle c = rx_now();
    dut->i_rgmii_rxd    = c.nibble;
    dut->i_rgmii_rx_ctl = c.dv;
    dut->eval();
    dut->i_rgmii_rxc = 1;
    dut->eval();
    rx_seam(rx_cycle);
}

void rx_fall() {
    RxCycle c = rx_now();
    dut->i_rgmii_rx_ctl = c.dv ^ c.error;
    dut->eval();
    dut->i_rgmii_rxc = 0;
    dut->eval();
    rx_cycle++;
}

// ═════════════════════════════════════════════════════════════
//  Transmit: the transmitter at the seam, and a PHY on the pins
// ═════════════════════════════════════════════════════════════

const int TX_CLK_PS = 10000;        // 100 MHz, as the harness says

int  tx_speed = 1;                  // CTRL.SPEED
// Transmit clock cycles in a quarter of the pin clock's period.
int  quarter() { return tx_speed == 1 ? 1 : 10; }

// ── The transmitter: a byte time put up, the next when it is taken ──
std::deque<std::vector<uint8_t>> tx_plan;       // packets still to send
std::vector<uint8_t> tx_sending;                // the one being sent
size_t tx_sent = 0;                             // how many of its bytes have been taken
int    tx_gap = 12;                             // byte times of gap still to put up
bool   tx_hold = false;                         // send nothing new: a change of speed is coming
std::deque<std::vector<uint8_t>> tx_expected;   // packets taken, for the pins to show
std::vector<uint8_t> tx_taking;                 // the packet being taken
int    tx_packets_ok = 0;

void tx_put_next() {
    if (tx_sent < tx_sending.size()) {
        dut->i_txd   = tx_sending[tx_sent++];
        dut->i_tx_en = 1;
        return;
    }
    dut->i_tx_en = 0;
    dut->i_txd   = (uint8_t)next_random();      // nothing reads it between packets
    if (tx_gap > 0) {
        tx_gap--;
    } else if (!tx_hold && !tx_plan.empty()) {
        tx_sending = tx_plan.front();
        tx_plan.pop_front();
        tx_sent = 0;
        tx_gap = random_in(1, 14);
        dut->i_txd   = tx_sending[tx_sent++];
        dut->i_tx_en = 1;
    }
}

bool tx_idle() { return tx_sent >= tx_sending.size() && !dut->i_tx_en; }

// ── The PHY on the transmit pins ─────────────────────────────
long tx_cycle = 0;
bool mon_on = false;                // the checks are standing
int  mon_edges = 0;                 // pin clock edges seen since they began to
int  pin_txc = 0, pin_txd = 0, pin_ctl = 0;
long last_change = 0;               // the cycle the data or control pins last changed
long last_edge = 0;                 // the cycle the pin clock last did
long last_ce = 0;
bool ce_seen = false;
bool phy_en = false;                // the control pin at the pin clock's last rise
int  phy_nibble = 0;                // the data pins there
bool phy_in_packet = false;
bool phy_have_low = false;
int  phy_low = 0;
std::vector<uint8_t> phy_bytes;

void mon_start() {
    mon_on = true;
    mon_edges = 0;
    ce_seen = false;
    phy_in_packet = false;
    phy_have_low = false;
    last_change = last_edge = tx_cycle - 1000;
}

// A nibble as the PHY has it once the pin clock has fallen.
void phy_nibble_time(bool en, bool er, int nibble) {
    check(!er,
          "the PHY reads an error: the control pin was %d at the pin clock's rise "
          "and %d at its fall",
          en, en ^ er);
    if (en) {
        if (!phy_in_packet) {
            phy_in_packet = true;
            phy_have_low = false;
            phy_bytes.clear();
        }
        if (!phy_have_low) {
            phy_low = nibble;
            phy_have_low = true;
        } else {
            phy_bytes.push_back((uint8_t)((nibble << 4) | phy_low));
            phy_have_low = false;
        }
        return;
    }
    if (!phy_in_packet)
        return;
    phy_in_packet = false;
    check(!phy_have_low, "a packet on the pins ended on half a byte");
    if (tx_expected.empty()) {
        check(false, "a packet of %zu bytes on the pins that the transmitter never sent",
              phy_bytes.size());
        return;
    }
    const std::vector<uint8_t>& want = tx_expected.front();
    size_t diff = 0;
    while (diff < want.size() && diff < phy_bytes.size() && want[diff] == phy_bytes[diff])
        diff++;
    bool same = (want.size() == phy_bytes.size() && diff == want.size());
    if (same) {
        tx_packets_ok++;
        pass++;
    } else if (diff < want.size() && diff < phy_bytes.size()) {
        check(false, "a packet of %zu bytes on the pins for one of %zu: byte %zu is %02x for %02x",
              phy_bytes.size(), want.size(), diff, phy_bytes[diff], want[diff]);
    } else {
        check(false, "a packet of %zu bytes on the pins for one of %zu",
              phy_bytes.size(), want.size());
    }
    tx_expected.pop_front();
}

void tx_pins() {
    int txc = dut->o_rgmii_txc, txd = dut->o_rgmii_txd, ctl = dut->o_rgmii_tx_ctl;
    int q = quarter();

    if (mon_on) {
        if (txd != pin_txd || ctl != pin_ctl) {
            check(tx_cycle - last_edge >= q,
                  "the data pins changed %ld cycles behind the pin clock's edge: "
                  "a quarter period is %d",
                  tx_cycle - last_edge, q);
            last_change = tx_cycle;
        }
        if (txc != pin_txc) {
            check(tx_cycle - last_change >= q,
                  "the pin clock %s %ld cycles behind a change of the data pins: "
                  "a quarter period is %d",
                  txc ? "rose" : "fell", tx_cycle - last_change, q);
            if (mon_edges >= 1)
                check(tx_cycle - last_edge == 2 * q, "the pin clock was %s for %ld cycles, not %d",
                      txc ? "low" : "high", tx_cycle - last_edge, 2 * q);
            if (txc) {
                phy_en     = ctl;
                phy_nibble = txd;
            } else if (mon_edges >= 1) {
                phy_nibble_time(phy_en, phy_en ^ (bool)ctl, phy_nibble);
            }
            last_edge = tx_cycle;
            mon_edges++;
        }
    }
    pin_txc = txc;
    pin_txd = txd;
    pin_ctl = ctl;
}

void tx_edge() {
    bool taken = dut->o_tx_ce;      // the byte time up through the cycle now ending
    if (taken) {
        if (mon_on) {
            if (ce_seen)
                check(tx_cycle - last_ce == 8 * quarter(),
                      "a byte time taken %ld cycles behind the last, not %d",
                      tx_cycle - last_ce, 8 * quarter());
            ce_seen = true;
        }
        last_ce = tx_cycle;
        if (dut->i_tx_en) {
            tx_taking.push_back(dut->i_txd);
        } else if (!tx_taking.empty()) {
            tx_expected.push_back(tx_taking);
            tx_taking.clear();
        }
    }

    dut->i_tx_clk = 1;
    dut->eval();
    tx_cycle++;
    tx_pins();
    if (taken)
        tx_put_next();
    dut->i_tx_clk = 0;
    dut->eval();
}

// ═════════════════════════════════════════════════════════════
//  The run
// ═════════════════════════════════════════════════════════════
long next_tx = 5000, next_rx = 7000;
long rx_half_ps = 20010;            // the PHY's clock, a little off 25 MHz
bool rxc_high = false;

void step() {
    if (next_tx <= next_rx) {
        now = next_tx;
        tx_edge();
        next_tx += TX_CLK_PS;
    } else {
        now = next_rx;
        if (!rxc_high) rx_rise(); else rx_fall();
        rxc_high = !rxc_high;
        next_rx += rx_half_ps;
    }
}

void run_tx_cycles(long cycles) {
    long until = tx_cycle + cycles;
    while (tx_cycle < until)
        step();
}

void plan_packets(int packets, int longest) {
    for (int i = 0; i < packets; i++)
        tx_plan.push_back(random_bytes(random_in(1, longest)));
}

// Send what is planned, at the speed given, with the checks standing
// from a few byte times after the speed is set.
void run_at_speed(int speed, const char* what) {
    tx_hold = true;
    mon_on = false;
    tx_speed = speed;
    dut->i_tx_speed = speed;
    rx_half_ps = (speed == 1) ? 20010 : 200100;
    run_tx_cycles(4 * 8 * 10);
    mon_start();
    run_tx_cycles(4 * 8 * 10);
    check(mon_edges >= 2, "%s: the pin clock is not running", what);

    tx_hold = false;
    size_t before = tx_packets_ok, planned = tx_plan.size();
    long limit = tx_cycle + (long)planned * 80 * 8 * quarter() + 100000;
    while ((!tx_plan.empty() || !tx_idle() || !tx_expected.empty() || !tx_taking.empty()) &&
           tx_cycle < limit)
        step();
    // The last packet's end is on the pins a byte time or two behind its
    // last byte being taken.
    run_tx_cycles(4 * 8 * quarter());
    check(tx_plan.empty() && tx_expected.empty(),
          "%s: %zu packets still to send and %zu sent that never reached the pins", what,
          tx_plan.size(), tx_expected.size());
    check(tx_packets_ok - before == planned, "%s: %zu of %zu packets reached the pins whole", what,
          tx_packets_ok - before, planned);
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_phy_rgmii_xc7_test;

    build_rx_script();

    dut->i_tx_clk = 0;
    dut->i_rgmii_rxc = 0;
    dut->i_rgmii_rxd = 0;
    dut->i_rgmii_rx_ctl = 0;
    dut->i_txd = 0;
    dut->i_tx_en = 0;
    dut->i_tx_speed = 1;
    dut->eval();

    check(dut->o_speeds == 0x3, "the speeds carried read %x, not 10 and 100", dut->o_speeds);

    plan_packets(150, 70);
    run_at_speed(1, "at 100 Mbit/s");
    plan_packets(40, 40);
    run_at_speed(0, "at 10 Mbit/s");
    plan_packets(60, 70);
    run_at_speed(1, "back at 100 Mbit/s");

    // Whatever of the receive script is left.
    rx_half_ps = 20010;
    long limit = rx_cycle + (long)rx_script.size() + 1000;
    while (!rx_done() && rx_cycle < limit)
        step();
    check(rx_done(), "the receive script did not finish");
    check(rx_in < 0, "packet %d was still being presented when the pins fell quiet", rx_in);
    for (size_t i = rx_next; i < rx_packets.size(); i++)
        check(false, "packet %zu (%s): never presented", i, rx_packets[i].what.c_str());
    check(rx_packets_ok == (int)rx_packets.size(), "%d of %zu packets presented whole",
          rx_packets_ok, rx_packets.size());

    printf("eth_phy_rgmii_xc7: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

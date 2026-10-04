// Verilator testbench for the I2C master
//
// The master drives a modelled bus: its two open-drain lines, a slave
// that follows the bus as a real one does, and a monitor that measures
// everything the master draws against the standard-mode minimums. The
// checks are the ones a consumer depends on: bytes arrive whole and in
// order with the right START, repeated START and STOP around them; an
// unanswered write ends in a STOP and says so; a read is acknowledged
// as asked; a bus that will not move fails the command instead of
// hanging it; and a reset that lands mid-transfer leaves the bus free.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include <string>
#include <vector>
#include "Vi2c_master_test.h"
#include "i2c_slave.h"

namespace {

Vi2c_master_test* dut;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    va_list ap; va_start(ap, fmt);
    printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
    va_end(ap); fail++;
}

// The harness runs the master on 4 MHz, so a cycle is 250 ns.
constexpr int NS_PER_CYCLE = 250;
constexpr long cycles(int ns) { return (ns + NS_PER_CYCLE - 1) / NS_PER_CYCLE; }

// Standard-mode minimums, in ns (NXP UM10204, table 10).
constexpr int T_LOW    = 4700;      // SCL low
constexpr int T_HIGH   = 4000;      // SCL high
constexpr int T_SU_STA = 4700;      // SCL high before a START's SDA fall
constexpr int T_HD_STA = 4000;      // START's SDA fall to SCL's fall
constexpr int T_SU_STO = 4000;      // SCL high before a STOP's SDA rise
constexpr int T_BUF    = 4700;      // STOP to the next START
constexpr int T_SU_DAT = 250;       // SDA settled before SCL rises
// The bus's own data hold minimum is zero. This is what a device needs
// SDA held past SCL's fall to still see SCL as low when SDA moves.
constexpr int T_HD_DAT = 300;

constexpr uint8_t SLAVE_ADDR = 0x3B;

// ── The slave ────────────────────────────────────────────────
I2cSlave slave(SLAVE_ADDR);

// ── The monitor ──────────────────────────────────────────────
// Watches the two lines, tells the slave what happened, and holds every
// interval to its minimum while enabled. A report names the two edges it
// measured between, with their directions and times, where on the bus
// they fell (which bit of which byte since the START), and the edges
// leading up to them. The testbench switches the monitor off around
// what it does to the bus itself.
struct Monitor {
    struct Edge { long t; std::string what; };

    bool scl = true, sda = true;
    bool enabled = true;
    int  violations = 0;
    int  starts = 0, stops = 0, scl_falls = 0;
    long t_scl_rise = -1000, t_scl_fall = -1000, t_sda_move = -1000;
    long t_start = -1000, t_stop = -1000;
    std::string sda_move_what = "SDA";
    bool start_hold_open = false;    // a START's SCL fall is still to come
    bool in_txn = false;             // between a START and a STOP
    long clocks = 0;                 // SCL rises since the START
    std::deque<Edge> recent;         // the last few edges, oldest first

    static double us(long t) { return t * NS_PER_CYCLE / 1000.0; }
    static const char* dir(bool now_high) { return now_high ? "0->1" : "1->0"; }

    // Clock c since the START, as a byte and a bit, most significant
    // first, with byte 0 the address.
    std::string clock_name(long c, const char* how) const {
        if (!in_txn) return "with no transaction open";
        if (c < 0) return "between the START and its first clock";
        char byte[32], buf[96];
        long b = c / 9, i = c % 9;
        if (b == 0) snprintf(byte, sizeof byte, "the address byte");
        else        snprintf(byte, sizeof byte, "byte %ld", b);
        if (i == 8) snprintf(buf, sizeof buf, "%s %s's acknowledge", how, byte);
        else        snprintf(buf, sizeof buf, "%s %s's bit %ld", how, byte, 7 - i);
        return buf;
    }

    static std::string bytes(long c) {
        long n = c / 9;
        if (n == 0) return "no byte";
        return std::to_string(n) + (n == 1 ? " byte" : " bytes");
    }

    void push(long t, const std::string& what) {
        recent.push_back({t, what});
        if (recent.size() > 6) recent.pop_front();
    }

    void report(const std::string& msg, const std::string& at) {
        if (!enabled || violations++ >= 8) return;
        printf("  timing: %s, %s\n    edges up to it:", msg.c_str(), at.c_str());
        for (const Edge& e : recent) printf(" %.2f us %s;", us(e.t), e.what.c_str());
        printf("\n");
    }

    void need(long t_from, const std::string& from, const std::string& to,
              int min_ns, const char* what, const std::string& at) {
        long got = now - t_from;
        if (!enabled || got >= cycles(min_ns)) return;
        char buf[256];
        snprintf(buf, sizeof buf, "%s is %.2f us, minimum %.2f: %s at %.2f us to %s at %.2f us",
                 what, us(got), min_ns / 1000.0, from.c_str(), us(t_from), to.c_str(), us(now));
        report(buf, at);
    }

    void observe(bool n_scl, bool n_sda) {
        bool scl_rise = n_scl && !scl, scl_fall = !n_scl && scl;
        bool sda_move = n_sda != sda;
        bool is_start = sda_move && n_scl && scl && !n_sda;
        bool is_stop  = sda_move && n_scl && scl && n_sda;

        std::string scl_what = std::string("SCL ") + dir(n_scl);
        std::string sda_what = std::string("SDA ") + dir(n_sda) +
                               (is_start ? " (START)" : is_stop ? " (STOP)" : "");
        if (scl_rise || scl_fall) push(now, scl_what);
        if (sda_move) push(now, sda_what);

        if (sda_move && (scl_rise || scl_fall))
            report(scl_what + " and " + sda_what + " in the same cycle",
                   scl_rise ? clock_name(clocks, "starting")
                            : clock_name(clocks - 1, "ending"));

        if (is_start) {
            std::string at = in_txn ? "at a repeated START after " + bytes(clocks)
                                    : std::string("at the START");
            need(t_scl_rise, "SCL 0->1", sda_what, T_SU_STA, "START setup", at);
            if (stops) need(t_stop, "the STOP before", sda_what, T_BUF, "bus free time", at);
            t_start = now; start_hold_open = true; starts++;
            in_txn = true; clocks = 0;
            slave.on_start();
        } else if (is_stop) {
            need(t_scl_rise, "SCL 0->1", sda_what, T_SU_STO, "STOP setup",
                 "at the STOP after " + bytes(clocks));
            t_stop = now; stops++; in_txn = false;
            slave.on_stop();
        } else if (sda_move) {
            need(t_scl_fall, "SCL 1->0", sda_what, T_HD_DAT, "data hold",
                 clock_name(clocks, "setting up"));
            t_sda_move = now; sda_move_what = sda_what;
        }
        if (scl_rise) {
            std::string at = clock_name(clocks, "starting");
            need(t_scl_fall, "SCL 1->0", scl_what, T_LOW, "SCL low", at);
            need(t_sda_move, sda_move_what, scl_what, T_SU_DAT, "data setup", at);
            t_scl_rise = now;
            if (in_txn) clocks++;
            slave.on_scl_rise(n_sda);
        }
        if (scl_fall) {
            need(t_scl_rise, "SCL 0->1", scl_what, T_HIGH, "SCL high",
                 clock_name(clocks - 1, "ending"));
            if (start_hold_open)
                need(t_start, "SDA 1->0 (START)", scl_what, T_HD_STA, "START hold",
                     "after the START");
            start_hold_open = false;
            t_scl_fall = now; scl_falls++;
            slave.on_scl_fall();
        }
        scl = n_scl; sda = n_sda;
    }
} mon;

// ── The bus ──────────────────────────────────────────────────
bool hold_scl = false, hold_sda = false;    // the testbench's own grip

void tick() {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
    slave.tick();
    bool scl = dut->o_scl && !hold_scl;
    bool sda = dut->o_sda && !slave.drive_low && !hold_sda;
    dut->i_scl = scl; dut->i_sda = sda;
    mon.observe(scl, sda);
    now++;
}

bool wait_ready() {
    for (int i = 0; i < 20000; i++) {
        if (dut->o_cmd_ready) return true;
        tick();
    }
    fail++; printf("FAIL: the master never became ready\n");
    return false;
}

struct Rsp { bool ok = false, nack = false, error = false; uint8_t rdata = 0; };

// One command, start to response.
Rsp cmd(bool start, bool stop, bool read, uint8_t wdata = 0, bool nack = false) {
    Rsp r;
    if (!wait_ready()) return r;
    dut->i_cmd_valid = 1; dut->i_cmd_start = start; dut->i_cmd_stop = stop;
    dut->i_cmd_read = read; dut->i_cmd_wdata = wdata; dut->i_cmd_nack = nack;
    tick();
    dut->i_cmd_valid = 0;
    for (int i = 0; i < 20000; i++) {
        if (dut->o_rsp_valid) {
            r.ok = true; r.nack = dut->o_rsp_nack; r.error = dut->o_rsp_error;
            r.rdata = dut->o_rsp_rdata;
            tick();
            return r;
        }
        tick();
    }
    fail++; printf("FAIL: no response to a command\n");
    return r;
}
Rsp wr(uint8_t b, bool start = false, bool stop = false) { return cmd(start, stop, false, b); }
Rsp rd(bool nack = false, bool stop = false)             { return cmd(false, stop, true, 0, nack); }

// A reset releases both lines at once wherever the bus stood, which is
// no symbol at all, so the monitor sits it out.
void reset() {
    mon.enabled = false;
    dut->i_rst = 1; dut->i_cmd_valid = 0;
    for (int i = 0; i < 4; i++) tick();
    dut->i_rst = 0;
    mon.enabled = true;
}

constexpr uint8_t ADDR_W = SLAVE_ADDR << 1, ADDR_R = (SLAVE_ADDR << 1) | 1;

} // namespace

int main() {
    dut = new Vi2c_master_test;
    dut->i_scl = 1; dut->i_sda = 1;

    // ── Reset on a free bus ──────────────────────────────────
    // Nothing holds SDA, so the clear is one START and one STOP: enough
    // to reset a slave caught receiving, with no bit clocked into it.
    reset();
    check(!dut->o_cmd_ready, "ready before the bus was cleared");
    wait_ready();
    check(mon.starts == 1 && mon.stops == 1,
          "clear on a free bus: %d STARTs and %d STOPs, expected one of each",
          mon.starts, mon.stops);
    check(mon.scl_falls == 1, "clear on a free bus clocked SCL %d times, expected once",
          mon.scl_falls);
    check(dut->o_scl && dut->o_sda, "lines not released after the clear");

    // ── A write ──────────────────────────────────────────────
    int starts = mon.starts, stops = mon.stops;
    Rsp r = wr(ADDR_W, true);
    check(r.ok && !r.nack && !r.error, "address byte: nack %d error %d", r.nack, r.error);
    check(mon.stops == stops, "a STOP inside an open transaction");
    r = wr(0x40); check(r.ok && !r.nack, "first data byte not acknowledged");
    r = wr(0x15); check(r.ok && !r.nack, "second data byte not acknowledged");
    r = wr(0xA5, false, true);
    check(r.ok && !r.nack, "last data byte not acknowledged");
    check(slave.addr_byte == ADDR_W, "slave saw address byte %02X, expected %02X",
          slave.addr_byte, ADDR_W);
    check(slave.written == std::vector<uint8_t>({0x40, 0x15, 0xA5}),
          "slave received %zu bytes, first %02X; expected 40 15 A5",
          slave.written.size(), slave.written.empty() ? 0 : slave.written[0]);
    check(mon.starts == starts + 1 && mon.stops == stops + 1,
          "write: %d STARTs and %d STOPs, expected one of each",
          mon.starts - starts, mon.stops - stops);
    check(slave.st == I2cSlave::IDLE, "slave not idle after the STOP");

    // ── A write nobody acknowledges ──────────────────────────
    // The master ends the transaction itself: the consumer is told, and
    // finds the bus free.
    stops = mon.stops;
    r = wr(0x20 << 1, true);
    check(r.ok && r.nack && !r.error, "unanswered address: nack %d error %d", r.nack, r.error);
    check(mon.stops == stops + 1, "no STOP after an unanswered address");
    check(dut->o_cmd_ready && dut->o_scl && dut->o_sda,
          "bus not free after an unanswered address");

    slave.ack_data = false;
    stops = mon.stops;
    r = wr(ADDR_W, true); check(r.ok && !r.nack, "address before a refused byte");
    r = wr(0x99);
    check(r.ok && r.nack, "a refused data byte was reported acknowledged");
    check(mon.stops == stops + 1, "no STOP after a refused data byte");
    slave.ack_data = true;

    // ── A read behind a repeated START ───────────────────────
    slave.written.clear();
    slave.to_send = {0xC3, 0x5A};
    starts = mon.starts; stops = mon.stops;
    r = wr(ADDR_W, true); check(r.ok && !r.nack, "read: address for the subaddress");
    r = wr(0x02);         check(r.ok && !r.nack, "read: subaddress");
    r = wr(ADDR_R, true); check(r.ok && !r.nack, "read: address behind the repeated START");
    check(mon.starts == starts + 2 && mon.stops == stops,
          "repeated START: %d STARTs and %d STOPs so far, expected 2 and 0",
          mon.starts - starts, mon.stops - stops);
    r = rd();
    check(r.ok && r.rdata == 0xC3, "first byte read %02X, expected C3", r.rdata);
    r = rd(true, true);
    check(r.ok && r.rdata == 0x5A, "second byte read %02X, expected 5A", r.rdata);
    check(!r.nack, "a read reported the master's own NACK as the slave's");
    check(slave.acks == 1 && slave.nacks == 1,
          "slave saw %d ACKs and %d NACKs for two bytes, expected one of each",
          slave.acks, slave.nacks);
    check(slave.written == std::vector<uint8_t>({0x02}), "subaddress not what the slave received");
    check(mon.stops == stops + 1 && slave.st == I2cSlave::IDLE, "read not closed by a STOP");

    // ── Reset while a slave holds SDA ────────────────────────
    // A reset mid-read leaves the slave driving a 0 with no clock to
    // move it on. The clear clocks it until it lets go, then STOPs.
    // The monitor takes the held line as how the bus already stands: SDA
    // falling under a high SCL would otherwise read as a START.
    slave.st = I2cSlave::SEND; slave.cur = 0x00; slave.bit = 2;
    slave.drive_low = true; slave.pending_in = 0;
    mon.sda = false; dut->i_sda = 0;
    starts = mon.starts; stops = mon.stops; mon.scl_falls = 0;
    reset();
    wait_ready();
    check(slave.st == I2cSlave::IDLE && !slave.drive_low, "slave still holds the bus after the clear");
    check(mon.starts == starts && mon.stops == stops + 1,
          "clear of a held bus: %d STARTs and %d STOPs, expected a STOP alone",
          mon.starts - starts, mon.stops - stops);
    check(mon.scl_falls <= 10, "clear took %d clocks, more than the nine it may", mon.scl_falls);
    r = wr(ADDR_W, true, true);
    check(r.ok && !r.nack && !r.error, "no transfer after clearing a held bus");

    // ── Reset in the middle of a byte written ────────────────
    // The slave is left three bits into a byte. The clear's START drops
    // it, so nothing the slave acknowledged is a byte nobody sent.
    slave.written.clear();
    r = wr(ADDR_W, true); check(r.ok && !r.nack, "address before the interrupted byte");
    wait_ready();
    dut->i_cmd_valid = 1; dut->i_cmd_start = 0; dut->i_cmd_stop = 0;
    dut->i_cmd_read = 0; dut->i_cmd_wdata = 0x0F; dut->i_cmd_nack = 0;
    tick();
    dut->i_cmd_valid = 0;
    for (int i = 0; i < 40 * 3 + 5; i++) tick();
    reset();
    wait_ready();
    check(slave.written.empty(), "slave took a byte cut short by a reset: %02X",
          slave.written.empty() ? 0 : slave.written[0]);
    check(slave.st == I2cSlave::IDLE, "slave not idle after a reset mid-byte");
    r = wr(ADDR_W, true, true);
    check(r.ok && !r.nack && !r.error, "no transfer after a reset mid-byte");

    // Everything the master drew up to here met the minimums.
    check(mon.violations == 0, "%d timing violations", mon.violations);

    // ── A clock that will not rise ───────────────────────────
    // The master does not wait on a stretched clock: the command fails.
    mon.enabled = false;
    hold_scl = true;
    r = wr(ADDR_W, true);
    check(r.ok && r.error, "no bus error with SCL held low");
    check(dut->o_scl && dut->o_sda, "lines not released after a bus error");
    hold_scl = false;
    for (int i = 0; i < 40; i++) tick();

    // ── SDA that never lets go ───────────────────────────────
    // The clear gives up after nine clocks, and a START then has no
    // high SDA to pull down.
    hold_sda = true;
    mon.scl_falls = 0;
    reset();
    mon.enabled = false;
    wait_ready();
    check(mon.scl_falls >= 9 && mon.scl_falls <= 10,
          "clear of a stuck bus clocked %d times, expected nine", mon.scl_falls);
    r = wr(ADDR_W, true);
    check(r.ok && r.error, "no bus error with SDA stuck low");
    hold_sda = false;

    // The bus works again once it is let go.
    reset();
    wait_ready();
    slave.written.clear();
    r = wr(ADDR_W, true); check(r.ok && !r.nack && !r.error, "address after the bus was freed");
    r = wr(0x7E, false, true);
    check(r.ok && !r.nack && slave.written == std::vector<uint8_t>({0x7E}),
          "no clean transfer after the bus was freed");
    check(mon.violations == 0, "%d timing violations after the bus was freed", mon.violations);

    printf("\ni2c_master: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

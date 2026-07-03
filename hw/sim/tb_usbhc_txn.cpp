// Verilator testbench for the Penumbra USB host transaction sequencer
//
// Stubs the two packet engines at their port contracts: each o_tx_start
// pulse is recorded (PID, field, length) and answered with i_tx_done after
// a fixed delay; the device's response is scheduled in bit times after the
// transaction's final host packet, presented as a receive window
// (i_rx_active) followed by the one-cycle classification pulse, exactly as
// usbhc_pkt_rx delivers it. Scenarios sweep the RESULT contract: ACK/NAK/
// STALL passthrough in both directions, hardware-ACK of good IN data,
// unacknowledged errored/oversized data, turnaround timeouts at both
// speeds, and the IN-only buffer store grant.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusbhc_txn.h"

// usb_pkg::usb_pid_e nibbles.
static const uint8_t PID_OUT   = 0x1;
static const uint8_t PID_IN    = 0x9;
static const uint8_t PID_SETUP = 0xD;
static const uint8_t PID_DATA0 = 0x3;
static const uint8_t PID_DATA1 = 0xB;
static const uint8_t PID_ACK   = 0x2;
static const uint8_t PID_NAK   = 0xA;
static const uint8_t PID_STALL = 0xE;

// penumbra_pkg contract encodings.
static const int TOK_SETUP = 0, TOK_OUT = 1, TOK_IN = 2;
static const int R_ACK = 0, R_NAK = 1, R_STALL = 2, R_TIMEOUT = 3,
                 R_ERROR = 4, R_OVERFLOW = 5;

// usb_speed_e and the seam's clocks-per-bit.
static const int SPEED_FS = 1, SPEED_LS = 2;
static int clocks_per_bit(int speed) { return speed == SPEED_LS ? 40 : 5; }

// Mirrors usbhc_txn's turnaround deadline for the timing checks.
static const int TURNAROUND_BT = 40;

struct TxPkt {
    uint8_t pid;
    uint16_t field;
    int len;
};

// The device's programmed response to the transaction's last host packet.
struct Resp {
    bool present = false;
    int delay_bt = 6;          // bit times from the final tx_done to the window
    uint8_t pid = PID_ACK;
    bool pid_ok = true;
    int len = 0;
    bool crc_ok = false;
    bool err = false;
    bool ovf = false;
};

struct TxnResult {
    std::vector<TxPkt> tx;
    bool done = false;
    int result = -1;
    int rxlen = -1;
    int rxtoggle = -1;
    bool accept_in_window = false;   // o_rx_accept while the response ran
    bool accept_while_idle = false;
    int cycles = 0;
};

// Run one transaction to o_done (or a cycle cap). resp_after_tx selects
// which host packet the device answers: 1 = the token (IN), 2 = the data
// stage (OUT/SETUP).
static TxnResult run_txn(Vusbhc_txn* dut, int pid_sel, int devaddr, int endp,
                         int toggle, int length, int speed,
                         const Resp& resp, int resp_after_tx) {
    TxnResult r;
    const int os = clocks_per_bit(speed);
    const int TX_DONE_DELAY = 30;    // stub packet-transmit duration, cycles
    const int WINDOW_CYCLES = 15;    // stub receive-window duration, cycles

    dut->i_speed = speed;
    dut->i_pid_sel = pid_sel;
    dut->i_devaddr = devaddr;
    dut->i_endpoint = endp;
    dut->i_toggle = toggle;
    dut->i_length = length;
    dut->i_start = 1;

    int tx_countdown = 0;            // pending i_tx_done pulse
    int tx_dones = 0;
    int resp_countdown = -1;         // cycles until the response window opens
    int window_left = 0;
    int rx_done_pulse = 0;

    for (r.cycles = 0; r.cycles < 20000; r.cycles++) {
        dut->i_tx_done = 0;
        dut->i_rx_done = 0;

        if (tx_countdown > 0 && --tx_countdown == 0) {
            dut->i_tx_done = 1;
            tx_dones++;
            if (resp.present && tx_dones == resp_after_tx)
                resp_countdown = resp.delay_bt * os;
        }
        if (resp_countdown > 0)
            resp_countdown--;
        if (resp_countdown == 0) {
            resp_countdown = -1;
            window_left = WINDOW_CYCLES;
        }
        if (window_left > 0) {
            dut->i_rx_active = 1;
            if (--window_left == 0)
                rx_done_pulse = 2;   // done fires the cycle after the window
        } else {
            dut->i_rx_active = 0;
        }
        if (rx_done_pulse > 0 && --rx_done_pulse == 0) {
            dut->i_rx_done = 1;
            dut->i_rx_pid = resp.pid;
            dut->i_rx_pid_ok = resp.pid_ok;
            dut->i_rx_len = resp.len;
            dut->i_rx_crc_ok = resp.crc_ok;
            dut->i_rx_err = resp.err;
            dut->i_rx_ovf = resp.ovf;
        }

        dut->i_clk = 0;
        dut->eval();
        if (dut->o_tx_start) {
            r.tx.push_back({(uint8_t)dut->o_tx_pid, (uint16_t)dut->o_tx_field,
                            (int)dut->o_tx_len});
            tx_countdown = TX_DONE_DELAY;
        }
        if (dut->i_rx_active && dut->o_rx_accept)
            r.accept_in_window = true;
        if (!dut->o_busy && dut->o_rx_accept)
            r.accept_while_idle = true;

        dut->i_clk = 1;
        dut->eval();
        dut->i_start = 0;
        if (dut->o_done) {
            r.done = true;
            r.result = dut->o_result;
            r.rxlen = dut->o_rxlen;
            r.rxtoggle = dut->o_rxtoggle;
        }
        if (r.done && tx_countdown == 0 && window_left == 0 && rx_done_pulse == 0)
            break;
    }
    // Settle any dangling stub state before the next scenario.
    dut->i_rx_active = 0;
    dut->i_tx_done = 0;
    dut->i_rx_done = 0;
    for (int i = 0; i < 4; i++) {
        dut->i_clk = 0; dut->eval();
        dut->i_clk = 1; dut->eval();
    }
    return r;
}

// ── Checking ─────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void check_txn(const char* name, const TxnResult& got, int result,
                      const std::vector<uint8_t>& tx_pids, int rxlen,
                      int rxtoggle, bool accept_in_window) {
    bool ok = got.done && !got.accept_while_idle &&
              got.result == result && got.rxlen == rxlen &&
              got.rxtoggle == rxtoggle &&
              got.accept_in_window == accept_in_window &&
              got.tx.size() == tx_pids.size();
    if (ok)
        for (size_t i = 0; i < tx_pids.size(); i++)
            ok = ok && got.tx[i].pid == tx_pids[i];
    if (ok) {
        g_pass++;
        return;
    }
    printf("MISMATCH %s:\n", name);
    printf("  want result=%d rxlen=%d rxtoggle=%d accept=%d tx={", result,
           rxlen, rxtoggle, accept_in_window);
    for (uint8_t p : tx_pids) printf(" %x", p);
    printf(" }\n  got  result=%d rxlen=%d rxtoggle=%d accept=%d done=%d"
           " idle_accept=%d tx={",
           got.result, got.rxlen, got.rxtoggle, got.accept_in_window,
           got.done, got.accept_while_idle);
    for (const TxPkt& p : got.tx) printf(" %x", p.pid);
    printf(" }\n");
    g_fail++;
}

int main() {
    Vusbhc_txn* dut = new Vusbhc_txn;

    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_start = 0;
    dut->i_tx_done = 0;
    dut->i_rx_done = 0;
    dut->i_rx_active = 0;
    dut->i_rx_pid = 0;
    dut->i_rx_pid_ok = 0;
    dut->i_rx_len = 0;
    dut->i_rx_crc_ok = 0;
    dut->i_rx_err = 0;
    dut->i_rx_ovf = 0;
    dut->i_speed = SPEED_FS;
    for (int i = 0; i < 2; i++) {
        dut->i_clk = 0; dut->eval();
        dut->i_clk = 1; dut->eval();
    }
    dut->i_rst = 0;

    Resp hs_ack{true, 6, PID_ACK, true, 0, false, false, false};
    Resp hs_nak = hs_ack;   hs_nak.pid = PID_NAK;
    Resp hs_stall = hs_ack; hs_stall.pid = PID_STALL;

    // OUT/SETUP: token + DATAx by toggle, then the handshake passthrough.
    check_txn("OUT ACK",
              run_txn(dut, TOK_OUT, 0x15, 0xE, 0, 8, SPEED_FS, hs_ack, 2),
              R_ACK, {PID_OUT, PID_DATA0}, 0, 0, false);
    check_txn("OUT toggle NAK",
              run_txn(dut, TOK_OUT, 0x15, 0xE, 1, 8, SPEED_FS, hs_nak, 2),
              R_NAK, {PID_OUT, PID_DATA1}, 0, 0, false);
    check_txn("SETUP ACK",
              run_txn(dut, TOK_SETUP, 0x00, 0x0, 0, 8, SPEED_FS, hs_ack, 2),
              R_ACK, {PID_SETUP, PID_DATA0}, 0, 0, false);
    check_txn("OUT STALL",
              run_txn(dut, TOK_OUT, 0x15, 0xE, 0, 8, SPEED_FS, hs_stall, 2),
              R_STALL, {PID_OUT, PID_DATA0}, 0, 0, false);

    // The token's field/length reach the transmitter intact.
    {
        TxnResult r = run_txn(dut, TOK_OUT, 0x3A, 0xA, 0, 13, SPEED_FS,
                              hs_ack, 2);
        bool ok = r.tx.size() == 2 &&
                  r.tx[0].field == ((0xA << 7) | 0x3A) && r.tx[1].len == 13;
        if (ok) g_pass++;
        else { printf("MISMATCH token field/len\n"); g_fail++; }
    }

    // Malformed OUT responses.
    Resp bad_nibble = hs_ack; bad_nibble.pid_ok = false;
    check_txn("OUT garbage handshake",
              run_txn(dut, TOK_OUT, 1, 0, 0, 4, SPEED_FS, bad_nibble, 2),
              R_ERROR, {PID_OUT, PID_DATA0}, 0, 0, false);
    Resp data_to_out{true, 6, PID_DATA0, true, 4, true, false, false};
    check_txn("OUT answered by DATAx",
              run_txn(dut, TOK_OUT, 1, 0, 0, 4, SPEED_FS, data_to_out, 2),
              R_ERROR, {PID_OUT, PID_DATA0}, 0, 0, false);
    Resp hs_err = hs_ack; hs_err.err = true;
    check_txn("OUT handshake with line error",
              run_txn(dut, TOK_OUT, 1, 0, 0, 4, SPEED_FS, hs_err, 2),
              R_ERROR, {PID_OUT, PID_DATA0}, 0, 0, false);

    // OUT silence: TIMEOUT, and not before the turnaround deadline.
    {
        Resp none;
        TxnResult r = run_txn(dut, TOK_OUT, 1, 0, 0, 4, SPEED_FS, none, 2);
        bool ok = r.done && r.result == R_TIMEOUT &&
                  r.cycles > TURNAROUND_BT * clocks_per_bit(SPEED_FS);
        if (ok) g_pass++;
        else {
            printf("MISMATCH OUT timeout: result=%d cycles=%d\n",
                   r.result, r.cycles);
            g_fail++;
        }
    }

    // IN: good data is hardware-acknowledged and reported.
    Resp in_data0{true, 6, PID_DATA0, true, 8, true, false, false};
    check_txn("IN DATA0 good",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_data0, 1),
              R_ACK, {PID_IN, PID_ACK}, 8, 0, true);
    Resp in_data1 = in_data0; in_data1.pid = PID_DATA1; in_data1.len = 3;
    check_txn("IN DATA1 good",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_data1, 1),
              R_ACK, {PID_IN, PID_ACK}, 3, 1, true);
    // Same again: back-to-back transactions reuse cleanly.
    check_txn("IN DATA0 again",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_data0, 1),
              R_ACK, {PID_IN, PID_ACK}, 8, 0, true);

    // IN: NAK/STALL pass through, nothing transmitted back.
    check_txn("IN NAK",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, hs_nak, 1),
              R_NAK, {PID_IN}, 0, 0, true);
    check_txn("IN STALL",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, hs_stall, 1),
              R_STALL, {PID_IN}, 0, 0, true);

    // IN: errored or malformed data is never acknowledged.
    Resp in_crcbad = in_data0; in_crcbad.crc_ok = false;
    check_txn("IN CRC error",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_crcbad, 1),
              R_ERROR, {PID_IN}, 8, 0, true);
    Resp in_lineerr = in_data0; in_lineerr.err = true;
    check_txn("IN line error",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_lineerr, 1),
              R_ERROR, {PID_IN}, 8, 0, true);
    Resp in_garbage = in_data0; in_garbage.pid_ok = false;
    check_txn("IN garbage PID",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_garbage, 1),
              R_ERROR, {PID_IN}, 0, 0, true);
    // A device ACKing an IN token is a protocol violation.
    check_txn("IN answered by ACK",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, hs_ack, 1),
              R_ERROR, {PID_IN}, 0, 0, true);

    // IN: past the buffer or past LENGTH is OVERFLOW, unacknowledged, with
    // the stored count still reported.
    Resp in_ovf = in_data0; in_ovf.ovf = true; in_ovf.len = 64;
    check_txn("IN buffer overflow",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 64, SPEED_FS, in_ovf, 1),
              R_OVERFLOW, {PID_IN}, 64, 0, true);
    Resp in_long = in_data0; in_long.len = 16;
    check_txn("IN past LENGTH",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_long, 1),
              R_OVERFLOW, {PID_IN}, 16, 0, true);

    // IN silence at both speeds: the deadline scales with the bit time.
    for (int speed : {SPEED_FS, SPEED_LS}) {
        Resp none;
        TxnResult r = run_txn(dut, TOK_IN, 1, 0, 0, 8, speed, none, 1);
        bool ok = r.done && r.result == R_TIMEOUT &&
                  r.cycles > TURNAROUND_BT * clocks_per_bit(speed);
        if (ok) g_pass++;
        else {
            printf("MISMATCH IN timeout speed=%d: result=%d cycles=%d\n",
                   speed, r.result, r.cycles);
            g_fail++;
        }
    }

    // A response arriving just inside the deadline still completes.
    Resp in_late = in_data0; in_late.delay_bt = TURNAROUND_BT - 5;
    check_txn("IN response just in time",
              run_txn(dut, TOK_IN, 0x15, 0x1, 0, 8, SPEED_FS, in_late, 1),
              R_ACK, {PID_IN, PID_ACK}, 8, 0, true);

    printf("usbhc_txn: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

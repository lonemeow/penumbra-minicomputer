// Verilator testbench for the Penumbra USB host MAC composition
//
// Plays the PHY side of the UTMI-shaped seam with line-realistic pacing
// (SYNC delay, bytes at the port speed's bit rate, turnaround gaps) and
// hands complete packets to the byte-level UsbDeviceSim responder — the
// shared device model the machine_sim PHY and the ISS's USBHC are meant
// to reuse. The register tier is played directly at the MAC's upward
// ports, including the dual-clock data buffer's two BRAM ports.
//
// The leaf cells own their protocol details (their own testbenches pin
// those); this bench pins what only the composition provides:
//
//   * whole transactions crossing the real seam end to end, every
//     XFER_STATUS result reached through actual wire images
//   * the receive store gate (a protocol-violating DATAx answering an
//     OUT must not scribble the transmit payload)
//   * the transmit arbiter: SOF/keep-alive markers under RUN, a start
//     arriving while a marker holds the transmitter (deferred, covered
//     by o_busy, never lost), markers surviving sustained traffic
//   * port plumbing: connect/speed detect feeding transaction and
//     marker pacing, the reset recipe, the resume transmit override
//
// The DUT's own assertions run under --assert and police transaction
// splitting and command routing; this bench checks the observable side.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vusbhc_mac_test.h"
#include "usb_device_sim.h"

// penumbra_pkg contract encodings.
static const int TOK_SETUP = 0, TOK_OUT = 1, TOK_IN = 2;
static const int R_ACK = 0, R_NAK = 1, R_STALL = 2, R_TIMEOUT = 3,
                 R_ERROR = 4, R_OVERFLOW = 5;

// usb_pkg encodings.
static const int SPEED_FS = 1, SPEED_LS = 2;
static const int LINE_SE0 = 0, LINE_J = 1, LINE_K = 2;
static const int OPMODE_NORMAL = 0, OPMODE_RAW = 2;

// Mirrors the wrapper's parameter overrides.
static const int CLKS_PER_MS = 4000;
static const int DEBOUNCE_CLKS = 30;

static Vusbhc_mac_test* dut;
static UsbDeviceSim dev;
static int g_pass = 0, g_fail = 0;

// ── The seam-side PHY model ──────────────────────────────────────────────
//
// Transmit: consumes the MAC's bytes at the line rate after a SYNC-time
// head start (only in the normal opmode — the raw recipes drive the line,
// not packets), and delivers each completed packet to the device model.
// Receive: paces the device's response back through the byte channel
// inside an i_rx_active window bracketed by SYNC and EOP time.
struct PhyModel {
    int cpb = 5;                   // clocks per line bit (5 FS, 40 LS)

    bool in_tx = false;
    int  tx_cd = 0;                // countdown to the next consume
    std::vector<uint8_t> tx_bytes;

    int  rx_wait = 0;              // countdown to the window opening
    bool rx_active = false;
    int  rx_sync = 0;              // window time before the first byte
    int  rx_pace = 0;              // countdown to the next byte
    int  rx_eop = 0;               // window time after the last byte
    size_t rx_idx = 0;
    std::vector<uint8_t> rx_pkt;

    int sync_clks() const { return 8 * cpb; }
    int byte_clks() const { return 8 * cpb; }

    void reset() {
        in_tx = false;
        tx_cd = 0;
        tx_bytes.clear();
        rx_wait = 0;
        rx_active = false;
        rx_idx = 0;
        rx_pkt.clear();
    }
};
static PhyModel phy;

// Device turnaround: bit times from the host packet's end to the response
// window opening. Comfortably inside usbhc_txn's 40-bit-time deadline.
static const int RESP_DELAY_BT = 8;

// Drives the seam inputs for this cycle; called before the clk-low eval so
// the DUT sees them settled, mirroring the leaf-cell benches.
static void phy_pre() {
    if (phy.in_tx)
        phy.tx_cd--;
    dut->i_tx_ready = phy.in_tx && (phy.tx_cd <= 0);

    dut->i_rx_valid = 0;
    dut->i_rx_error = 0;
    if (phy.rx_wait > 0 && --phy.rx_wait == 0) {
        phy.rx_active = true;
        phy.rx_sync = phy.sync_clks();
        phy.rx_pace = 0;
        phy.rx_eop = 2 * phy.cpb;
        phy.rx_idx = 0;
    }
    if (phy.rx_active) {
        if (phy.rx_sync > 0) {
            phy.rx_sync--;
        } else if (phy.rx_idx < phy.rx_pkt.size()) {
            if (phy.rx_pace <= 0) {
                dut->i_rx_data = phy.rx_pkt[phy.rx_idx++];
                dut->i_rx_valid = 1;
                phy.rx_pace = phy.byte_clks() - 1;
            } else {
                phy.rx_pace--;
            }
        } else if (phy.rx_eop > 0) {
            phy.rx_eop--;         // EOP time, then the window closes
        } else {
            phy.rx_active = false;
        }
    }
    dut->i_rx_active = phy.rx_active;
}

// Samples the transmit channel after the clk-low eval: packet bracketing
// follows o_tx_valid, consumes follow the pre-set i_tx_ready.
static void phy_collect() {
    bool valid = dut->o_tx_valid && dut->o_opmode == OPMODE_NORMAL;
    if (valid && !phy.in_tx) {
        phy.in_tx = true;
        phy.tx_bytes.clear();
        phy.tx_cd = phy.sync_clks();
        dut->i_tx_ready = 0;      // the head start begins next cycle
    } else if (valid && dut->i_tx_ready) {
        phy.tx_bytes.push_back(dut->o_tx_data);
        phy.tx_cd = phy.byte_clks();
    } else if (!valid && phy.in_tx) {
        phy.in_tx = false;
        dev.host_packet(phy.tx_bytes);
        if (dev.has_response()) {
            phy.rx_pkt = dev.take_response();
            phy.rx_wait = RESP_DELAY_BT * phy.cpb;
        }
    }
}

// ── Clocking + the data buffer's two BRAM ports ──────────────────────────

static uint8_t g_buf[128];

static void tick() {
    phy_pre();
    dut->i_clk = 0;
    dut->eval();
    phy_collect();
    uint8_t raddr = (uint8_t)(dut->o_buf_raddr & 0x7f);
    bool    we    = dut->o_buf_we;
    uint8_t waddr = (uint8_t)(dut->o_buf_waddr & 0x7f);
    uint8_t wdata = dut->o_buf_wdata;
    dut->i_clk = 1;
    dut->eval();
    if (we)
        g_buf[waddr] = wdata;
    dut->i_buf_rdata = g_buf[raddr];
}

// ── Checking ─────────────────────────────────────────────────────────────

static void expect(const char* name, bool ok) {
    if (ok) {
        g_pass++;
        return;
    }
    printf("FAIL %s\n", name);
    g_fail++;
}

struct TxnOut {
    bool done = false;
    bool busy_after_start = false;
    int result = -1;
    int rxlen = -1;
    int rxtoggle = -1;
};

// Launches one transaction and runs to o_done (or the cycle cap). The
// request fields stay driven throughout — the register tier holds them
// stable while busy, and a deferred start samples them at launch time.
static TxnOut run_txn(int pid_sel, int addr, int endp, int toggle, int len,
                      int cap) {
    TxnOut r;
    dut->i_pid_sel = pid_sel;
    dut->i_devaddr = addr;
    dut->i_endpoint = endp;
    dut->i_toggle = toggle;
    dut->i_length = len;
    dut->i_start = 1;
    tick();
    dut->i_start = 0;
    r.busy_after_start = dut->o_busy;
    for (int i = 0; i < cap; i++) {
        tick();
        if (dut->o_done) {
            r.done = true;
            r.result = dut->o_result;
            r.rxlen = dut->o_rxlen;
            r.rxtoggle = dut->o_rxtoggle;
            break;
        }
    }
    // Drain the line: the final packet's EOP crosses the phy model (and
    // reaches the device's log) a few cycles after o_done.
    for (int i = 0; i < 8 * phy.cpb; i++)
        tick();
    return r;
}

static void expect_result(const char* name, const TxnOut& r, int want) {
    if (r.done && r.busy_after_start && r.result == want) {
        g_pass++;
        return;
    }
    printf("FAIL %s: done=%d busy=%d result=%d want=%d\n", name, r.done,
           r.busy_after_start, r.result, want);
    g_fail++;
}

static void run(int cycles) {
    for (int i = 0; i < cycles; i++)
        tick();
}

// Frame numbers must advance by one per marker while the bus is otherwise
// idle; under traffic a marker may coalesce forward, so only monotonic
// advance (mod 2^11) is required there.
static bool frames_advance(const std::vector<uint16_t>& f, bool exact) {
    for (size_t i = 1; i < f.size(); i++) {
        uint16_t step = (uint16_t)((f[i] - f[i - 1]) & 0x7ff);
        if (exact ? (step != 1) : (step == 0))
            return false;
    }
    return true;
}

int main() {
    // The device model's wire arithmetic must reproduce the golden values
    // sw/tools/usb_crc.py prints before it is trusted to judge the RTL.
    if (UsbDeviceSim::token_crc5(0x715) != 0x1D ||
        UsbDeviceSim::token_crc5(0x53A) != 0x07 ||
        UsbDeviceSim::data_crc16({0xde, 0xad, 0xbe, 0xef}) != 0x3e64 ||
        UsbDeviceSim::pid_byte(UsbDeviceSim::PID_SOF) != 0xa5) {
        printf("usbhc_mac: UsbDeviceSim reference anchors failed\n");
        return 1;
    }

    dut = new Vusbhc_mac_test;
    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_start = 0;
    dut->i_pid_sel = 0;
    dut->i_devaddr = 0;
    dut->i_endpoint = 0;
    dut->i_toggle = 0;
    dut->i_length = 0;
    dut->i_run = 0;
    dut->i_power = 0;
    dut->i_reset = 0;
    dut->i_suspend = 0;
    dut->i_resume = 0;
    dut->i_tx_ready = 0;
    dut->i_rx_data = 0;
    dut->i_rx_valid = 0;
    dut->i_rx_active = 0;
    dut->i_rx_error = 0;
    dut->i_line_state = LINE_SE0;
    dut->i_caps = 0x3;   // {hs=0, fs, ls}, the usb_phy_ecp5 ceiling
    dut->i_buf_rdata = 0;
    tick();
    tick();
    dut->i_rst = 0;

    const int FS_CAP = 20000, LS_CAP = 120000;

    // ── Attach at full speed ─────────────────────────────────────────
    dut->i_power = 1;
    dut->i_line_state = LINE_J;       // D+ pull-up: a full-speed device
    bool change_seen = false;
    for (int i = 0; i < DEBOUNCE_CLKS * 4 && !dut->o_connect; i++) {
        tick();
        change_seen |= dut->o_port_change;
    }
    expect("attach-fs connect", dut->o_connect);
    expect("attach-fs speed", dut->o_port_speed == SPEED_FS);
    expect("attach-fs change event", change_seen);
    expect("attach-fs power", dut->o_port_power);
    expect("attach-fs not yet enabled", !dut->o_enabled);
    phy.cpb = 5;

    // ── Bus reset recipe ─────────────────────────────────────────────
    dut->i_reset = 1;
    run(4);
    expect("reset drive state", dut->o_reset_active &&
           dut->o_xcvr_sel == 0 && dut->o_term_sel == 0 &&
           dut->o_opmode == OPMODE_RAW);
    run(200);                          // software times the >=10 ms hold
    dut->i_reset = 0;
    run(4);
    expect("reset enables the port", dut->o_enabled);
    expect("post-reset traffic state", dut->o_xcvr_sel == SPEED_FS &&
           dut->o_term_sel == 1 && dut->o_opmode == OPMODE_NORMAL);

    // ── SETUP with payload, device ACKs ──────────────────────────────
    std::vector<uint8_t> setup_pl{0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                                  0x40, 0x00};
    for (size_t i = 0; i < setup_pl.size(); i++)
        g_buf[i] = setup_pl[i];
    dev.out_response = UsbDeviceSim::RSP_ACK;
    expect_result("setup-ack",
                  run_txn(TOK_SETUP, 0x00, 0, 0, 8, FS_CAP), R_ACK);
    expect("setup token on the wire",
           !dev.tokens.empty() &&
           dev.tokens.back().pid == UsbDeviceSim::PID_SETUP &&
           dev.tokens.back().addr == 0x00 && dev.tokens.back().endp == 0);
    expect("setup payload intact", dev.out_payload == setup_pl);

    // ── OUT: NAK / STALL / silence / protocol-violating DATA ────────
    dev.out_response = UsbDeviceSim::RSP_NAK;
    expect_result("out-nak", run_txn(TOK_OUT, 0x05, 2, 1, 8, FS_CAP), R_NAK);
    dev.out_response = UsbDeviceSim::RSP_STALL;
    expect_result("out-stall", run_txn(TOK_OUT, 0x05, 2, 1, 8, FS_CAP),
                  R_STALL);
    dev.out_response = UsbDeviceSim::RSP_SILENT;
    expect_result("out-timeout", run_txn(TOK_OUT, 0x05, 2, 1, 8, FS_CAP),
                  R_TIMEOUT);

    // A DATAx where a handshake belongs is ERROR, and the store gate must
    // keep it away from the transmit payload the buffer still holds.
    for (size_t i = 0; i < 8; i++)
        g_buf[i] = (uint8_t)(0xA0 | i);
    dev.out_response = UsbDeviceSim::RSP_DATA;
    expect_result("out-data-violation",
                  run_txn(TOK_OUT, 0x05, 2, 0, 8, FS_CAP), R_ERROR);
    bool intact = true;
    for (size_t i = 0; i < 8; i++)
        intact &= (g_buf[i] == (uint8_t)(0xA0 | i));
    expect("store gate holds on OUT", intact);
    dev.out_response = UsbDeviceSim::RSP_ACK;

    // ── IN: NAK / STALL / data with hardware ACK / overflow ─────────
    dev.in_response = UsbDeviceSim::RSP_NAK;
    expect_result("in-nak", run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP), R_NAK);
    dev.in_response = UsbDeviceSim::RSP_STALL;
    expect_result("in-stall", run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP),
                  R_STALL);

    std::vector<uint8_t> in_pl{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88};
    dev.in_response = UsbDeviceSim::RSP_DATA;
    dev.in_payload = in_pl;
    dev.in_toggle = true;
    memset(g_buf, 0, sizeof(g_buf));
    int acks_before = dev.acks_seen;
    TxnOut in_r = run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP);
    expect_result("in-data", in_r, R_ACK);
    expect("in-data length", in_r.rxlen == 8);
    expect("in-data toggle", in_r.rxtoggle == 1);
    bool stored = true;
    for (size_t i = 0; i < in_pl.size(); i++)
        stored &= (g_buf[i] == in_pl[i]);
    expect("in-data stored", stored);
    expect("in-data hardware ACK", dev.acks_seen == acks_before + 1);

    // The accept bound comes from the request's LENGTH: an intact but
    // longer answer is OVERFLOW and goes unacknowledged.
    acks_before = dev.acks_seen;
    expect_result("in-overflow", run_txn(TOK_IN, 0x05, 1, 0, 4, FS_CAP),
                  R_OVERFLOW);
    expect("in-overflow unacknowledged", dev.acks_seen == acks_before);
    dev.in_response = UsbDeviceSim::RSP_NAK;

    // ── SOF markers on an idle bus ───────────────────────────────────
    dev.sof_frames.clear();
    dut->i_run = 1;
    run(CLKS_PER_MS * 3 + CLKS_PER_MS / 2);
    expect("sof markers emitted", dev.sof_frames.size() >= 2);
    expect("sof frames consecutive", frames_advance(dev.sof_frames, true));

    // ── A start arriving while a marker holds the transmitter ───────
    // Waits for a marker's PID byte to cross the seam, then launches: the
    // start must be deferred behind the marker (o_busy covering it, per
    // the register contract) and still complete — never dropped. The
    // DUT's assertions police that the marker is not split.
    bool marker_hit = false;
    for (int i = 0; i < CLKS_PER_MS * 3; i++) {
        tick();
        if (phy.in_tx && phy.tx_bytes.size() == 1 &&
            phy.tx_bytes[0] == 0xa5) {
            marker_hit = true;
            break;
        }
    }
    expect("marker in flight found", marker_hit);
    expect_result("start deferred behind marker",
                  run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP), R_NAK);

    // ── A start landing in the release window ───────────────────────
    // Between a marker's completion and the grant's release there is a
    // cycle where the transmitter and sequencer are idle but ownership
    // still stands — and o_busy reads 0, so the register tier may start
    // right there. The start must defer or launch, never vanish. The
    // pulse is aimed by predicting the marker's end from its last byte
    // (an FS SOF token is three bytes).
    bool tail_hit = false;
    for (int i = 0; i < CLKS_PER_MS * 3; i++) {
        tick();
        if (phy.in_tx && phy.tx_bytes.size() == 3 &&
            phy.tx_bytes[0] == 0xa5) {
            tail_hit = true;
            break;
        }
    }
    expect("marker tail found", tail_hit);
    expect_result("start in the release window",
                  run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP), R_NAK);

    // ── Markers under sustained traffic ──────────────────────────────
    dev.sof_frames.clear();
    dev.in_response = UsbDeviceSim::RSP_DATA;
    dev.in_payload = in_pl;
    bool traffic_ok = true;
    for (int n = 0; n < 12; n++) {
        TxnOut r = run_txn(TOK_IN, 0x05, 1, (n & 1), 8, FS_CAP);
        traffic_ok &= r.done && r.result == R_ACK;
    }
    expect("traffic under markers completes", traffic_ok);
    expect("markers survive traffic", dev.sof_frames.size() >= 2);
    expect("frames advance under traffic",
           frames_advance(dev.sof_frames, false));
    dut->i_run = 0;
    dev.in_response = UsbDeviceSim::RSP_NAK;

    // ── RUN cleared while a marker is mid-flight ─────────────────────
    // The frame timer walks away immediately (its stop contract) and
    // frm_req falls with no completion strobe; the disowned marker keeps
    // draining in the packet transmitter. A start arriving right then
    // must wait out the drain — not collide with it — and still complete.
    dut->i_run = 1;
    bool marker_hit2 = false;
    for (int i = 0; i < CLKS_PER_MS * 3; i++) {
        tick();
        if (phy.in_tx && phy.tx_bytes.size() == 1 &&
            phy.tx_bytes[0] == 0xa5) {
            marker_hit2 = true;
            break;
        }
    }
    expect("marker in flight found (run-clear)", marker_hit2);
    dut->i_run = 0;
    expect_result("start after disowned marker",
                  run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP), R_NAK);

    // ── Detach, then a low-speed attach with keep-alives ─────────────
    dut->i_line_state = LINE_SE0;
    for (int i = 0; i < DEBOUNCE_CLKS * 4 && dut->o_connect; i++)
        tick();
    expect("detach disconnects", !dut->o_connect && !dut->o_enabled);

    dut->i_line_state = LINE_K;       // D- pull-up: a low-speed device
    for (int i = 0; i < DEBOUNCE_CLKS * 4 && !dut->o_connect; i++)
        tick();
    expect("attach-ls speed", dut->o_connect &&
           dut->o_port_speed == SPEED_LS);
    phy.cpb = 40;

    dut->i_reset = 1;
    run(200);
    dut->i_reset = 0;
    run(4);

    int ka_before = dev.keepalives;
    dut->i_run = 1;
    run(CLKS_PER_MS * 3);
    expect("ls keep-alives emitted", dev.keepalives >= ka_before + 2);
    expect_result("ls in-nak", run_txn(TOK_IN, 0x03, 0, 0, 8, LS_CAP),
                  R_NAK);
    dut->i_run = 0;
    // Quiesce before the suspend recipe, as software must: RUN just
    // cleared, and a disowned keep-alive may still be draining.
    run(CLKS_PER_MS);

    // ── Resume override and suspend passthrough ──────────────────────
    dut->i_suspend = 1;
    tick();
    expect("suspend status", dut->o_suspended);
    dut->i_resume = 1;
    run(4);
    expect("resume drives the channel", dut->o_tx_valid &&
           dut->o_tx_data == 0x00 && dut->o_opmode == OPMODE_RAW);
    dut->i_resume = 0;
    dut->i_suspend = 0;
    run(4);
    expect("resume releases the channel", !dut->o_tx_valid);

    // ── A start arriving during the resume recipe ────────────────────
    // bus_driven blocks the launch: the start defers (o_busy covering
    // it), stays deferred for the recipe's whole hold, and runs only
    // once the recipe ends. The early window exceeds a whole low-speed
    // transaction, so a leaked launch would show up as a done.
    dut->i_resume = 1;
    run(4);
    dut->i_pid_sel = TOK_IN;
    dut->i_devaddr = 0x03;
    dut->i_endpoint = 0;
    dut->i_toggle = 0;
    dut->i_length = 8;
    dut->i_start = 1;
    tick();
    dut->i_start = 0;
    bool deferred = dut->o_busy;
    bool early_done = false;
    for (int i = 0; i < 4000; i++) {
        tick();
        early_done |= dut->o_done;
    }
    expect("start defers during resume", deferred && !early_done);
    dut->i_resume = 0;
    TxnOut dr;
    for (int i = 0; i < LS_CAP; i++) {
        tick();
        if (dut->o_done) {
            dr.done = true;
            dr.result = dut->o_result;
            break;
        }
    }
    expect("deferred start runs after resume",
           dr.done && dr.result == R_NAK);

    // A clean run never garbles a wire image.
    expect("no CRC failures at the device",
           dev.crc_failures == 0 && dev.pid_failures == 0);

    printf("usbhc_mac: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

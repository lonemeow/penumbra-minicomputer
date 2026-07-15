// Verilator testbench for the Penumbra USB MAC + sim-PHY stack
//
// The MAC's testbench drove the UTMI seam itself; here the seam is wired
// for real — usbhc_mac against usb_phy_sim — and the bench only touches
// what the surrounding machine will: the register-tier surface above and
// the byte-level device port below, where UsbDeviceSim answers through
// the credit handshake. Every scenario therefore proves the two tiers
// mate: transaction results, buffer traffic, port recipes, and frame
// markers all cross the seam in both directions at real bit-time pacing.
// The device owns its turnaround delay, as the contract assigns it.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vusbhc_stack_test.h"
#include "usb_device_sim.h"

// penumbra_pkg contract encodings.
static const int TOK_SETUP = 0, TOK_OUT = 1, TOK_IN = 2;
static const int R_ACK = 0, R_NAK = 1, R_TIMEOUT = 3;

// usb_pkg encodings.
static const int SPEED_FS = 1, SPEED_LS = 2;

// Mirrors the wrapper's parameter overrides.
static const int CLKS_PER_MS = 4000;
static const int DEBOUNCE_CLKS = 30;

static Vusbhc_stack_test* dut;
static UsbDeviceSim dev;
static int g_pass = 0, g_fail = 0;

static int clocks_per_bit(int speed) { return speed == SPEED_LS ? 40 : 5; }
static int g_cpb = 5;

// Device turnaround, bit times from packet end to the response's first
// byte — inside the MAC's 40-bit-time deadline with the PHY's SYNC time.
static const int RESP_DELAY_BT = 8;

// ── The C++ device bridge on the credit port ─────────────────────────────

static std::vector<uint8_t> g_host_pkt;
static std::vector<uint8_t> g_resp;
static size_t g_resp_idx = 0;
static int g_resp_wait = 0;
static bool g_presenting = false;
static int g_keepalives_evt = 0;

static void bridge_pre() {
    if (g_resp_wait > 0 && --g_resp_wait == 0) {
        g_presenting = true;
        g_resp_idx = 0;
    }
    dut->i_rx_valid = g_presenting;
    if (g_presenting) {
        dut->i_rx_data = g_resp[g_resp_idx];
        dut->i_rx_last = (g_resp_idx == g_resp.size() - 1);
    }
}

static void bridge_post() {
    if (dut->o_pkt_valid)
        g_host_pkt.push_back(dut->o_pkt_data);
    if (dut->o_pkt_end) {
        dev.host_packet(g_host_pkt);
        g_host_pkt.clear();
        if (dev.has_response()) {
            g_resp = dev.take_response();
            g_resp_wait = RESP_DELAY_BT * g_cpb;
        }
    }
    if (dut->o_keepalive) {
        g_keepalives_evt++;
        dev.host_packet({0xa5});   // the bare EOP the wire would carry
    }
    if (dut->o_rx_ready) {
        g_resp_idx++;
        if (g_resp_idx >= g_resp.size())
            g_presenting = false;
    }
}

// ── Clocking + the data buffer's two BRAM ports ──────────────────────────

static uint8_t g_buf[128];

static void tick() {
    bridge_pre();
    dut->i_clk = 0;
    dut->eval();
    // Sample the device port before the edge: its pulses are
    // combinational from the pre-edge state and last one cycle.
    bridge_post();
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

static void run(int cycles) {
    for (int i = 0; i < cycles; i++)
        tick();
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
    int result = -1;
    int rxlen = -1;
    int rxtoggle = -1;
};

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
    run(8 * g_cpb);   // drain the wire past o_done
    return r;
}

static void expect_result(const char* name, const TxnOut& r, int want) {
    if (r.done && r.result == want) {
        g_pass++;
        return;
    }
    printf("FAIL %s: done=%d result=%d want=%d\n", name, r.done, r.result,
           want);
    g_fail++;
}

int main() {
    dut = new Vusbhc_stack_test;
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
    dut->i_rx_valid = 0;
    dut->i_rx_data = 0;
    dut->i_rx_last = 0;
    dut->i_dev_connect = 0;
    dut->i_dev_speed = SPEED_FS;
    dut->i_buf_rdata = 0;
    tick();
    tick();
    dut->i_rst = 0;

    const int FS_CAP = 20000, LS_CAP = 120000;

    // ── Attach at full speed through the sim pull-up ─────────────────
    dut->i_power = 1;
    dut->i_dev_connect = 1;
    dut->i_dev_speed = SPEED_FS;
    for (int i = 0; i < DEBOUNCE_CLKS * 6 && !dut->o_connect; i++)
        tick();
    expect("attach-fs", dut->o_connect && dut->o_port_speed == SPEED_FS);
    g_cpb = clocks_per_bit(SPEED_FS);

    // ── Bus reset, visible at the device port ────────────────────────
    dut->i_reset = 1;
    run(8);
    expect("reset reaches the device", dut->o_bus_reset);
    run(200);
    dut->i_reset = 0;
    run(8);
    expect("reset enables the port", dut->o_enabled && !dut->o_bus_reset);

    // ── Control traffic across the mated seam ────────────────────────
    std::vector<uint8_t> setup_pl{0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                                  0x40, 0x00};
    for (size_t i = 0; i < setup_pl.size(); i++)
        g_buf[i] = setup_pl[i];
    dev.out_response = UsbDeviceSim::RSP_ACK;
    expect_result("setup-ack",
                  run_txn(TOK_SETUP, 0x00, 0, 0, 8, FS_CAP), R_ACK);
    expect("setup payload across the seam", dev.out_payload == setup_pl &&
           !dev.tokens.empty() &&
           dev.tokens.back().pid == UsbDeviceSim::PID_SETUP);

    std::vector<uint8_t> in_pl{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88};
    dev.in_response = UsbDeviceSim::RSP_DATA;
    dev.in_payload = in_pl;
    dev.in_toggle = true;
    memset(g_buf, 0, sizeof(g_buf));
    int acks_before = dev.acks_seen;
    TxnOut in_r = run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP);
    expect_result("in-data", in_r, R_ACK);
    bool stored = true;
    for (size_t i = 0; i < in_pl.size(); i++)
        stored &= (g_buf[i] == in_pl[i]);
    expect("in-data delivered", in_r.rxlen == 8 && in_r.rxtoggle == 1 &&
           stored);
    expect("hardware ACK across the seam", dev.acks_seen == acks_before + 1);

    dev.in_response = UsbDeviceSim::RSP_NAK;
    expect_result("in-nak", run_txn(TOK_IN, 0x05, 1, 0, 8, FS_CAP), R_NAK);

    // A silent device: the turnaround deadline against the PHY's pacing.
    dev.out_response = UsbDeviceSim::RSP_SILENT;
    expect_result("out-timeout", run_txn(TOK_OUT, 0x05, 2, 1, 8, FS_CAP),
                  R_TIMEOUT);
    dev.out_response = UsbDeviceSim::RSP_ACK;

    // ── Frame markers ────────────────────────────────────────────────
    dev.sof_frames.clear();
    dut->i_run = 1;
    run(CLKS_PER_MS * 3 + CLKS_PER_MS / 2);
    dut->i_run = 0;
    bool consecutive = dev.sof_frames.size() >= 2;
    for (size_t i = 1; i < dev.sof_frames.size(); i++)
        consecutive &= (((dev.sof_frames[i] - dev.sof_frames[i - 1]) &
                         0x7ff) == 1);
    expect("sof markers across the stack", consecutive);
    run(CLKS_PER_MS);

    // ── Detach, low-speed attach, keep-alives ────────────────────────
    dut->i_dev_connect = 0;
    for (int i = 0; i < DEBOUNCE_CLKS * 6 && dut->o_connect; i++)
        tick();
    expect("detach", !dut->o_connect);
    dut->i_dev_connect = 1;
    dut->i_dev_speed = SPEED_LS;
    for (int i = 0; i < DEBOUNCE_CLKS * 6 && !dut->o_connect; i++)
        tick();
    expect("attach-ls", dut->o_connect && dut->o_port_speed == SPEED_LS);
    g_cpb = clocks_per_bit(SPEED_LS);
    dut->i_reset = 1;
    run(200);
    dut->i_reset = 0;
    run(8);

    int ka_before = g_keepalives_evt;
    dut->i_run = 1;
    run(CLKS_PER_MS * 3);
    dut->i_run = 0;
    expect("ls keep-alive events", g_keepalives_evt >= ka_before + 2 &&
           dev.keepalives == g_keepalives_evt);
    run(CLKS_PER_MS);

    expect_result("ls in-nak", run_txn(TOK_IN, 0x03, 0, 0, 8, LS_CAP),
                  R_NAK);

    // ── Resume reaches the device port ───────────────────────────────
    dut->i_resume = 1;
    run(8);
    expect("resume reaches the device", dut->o_dev_resume);
    dut->i_resume = 0;
    run(8);
    expect("resume releases", !dut->o_dev_resume);

    expect("no wire corruption", dev.crc_failures == 0 &&
           dev.pid_failures == 0);

    printf("usbhc_stack: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

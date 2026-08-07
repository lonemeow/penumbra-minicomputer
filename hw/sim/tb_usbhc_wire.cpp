// Verilator testbench for wire-level USB enumeration with a strict device
//
// The full host stack (usbhc + usb_phy_ecp5) against a device transceiver
// (a second usb_phy_ecp5) on a resolved wire, with a spec-strict device
// model behind it: deaf to all traffic until it has measured a valid bus
// reset — a continuous SE0 of at least the spec detection minimum — on
// its own line tap.  This is the coverage the byte-level device tests
// structurally lack (their device model is born responsive): everything
// the device learns here, it learns from the wire.  Pinned behaviors:
// connect detection off the idle polarity, deafness before reset, SOF
// markers as real wire packets, the reset recipe producing a
// device-visible continuous SE0, and enumeration first contact (SETUP
// GET_DESCRIPTOR, data stage, status stage) — the sequence whose fused
// SOF+token corruption this bench first reproduced.
//
// The CPU and USB clocks run at the 5:2 unit ratio of tb_usbhc_dev.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusbhc_wire_test.h"
#include "usb_device_sim.h"

static const uint32_t R_PORT_STATUS = 0x0C, R_PORT_CTRL = 0x10,
                      R_TOKEN = 0x18, R_XFER_CTRL = 0x1C,
                      R_XFER_STATUS = 0x20, R_IRQ_STATUS = 0x04,
                      R_IRQ_ENABLE = 0x08, R_DATA = 0x40;
static const int TOK_SETUP = 0, TOK_OUT = 1, TOK_IN = 2;
static const int RES_ACK = 0, RES_TIMEOUT = 3;
static const uint32_t PC_POWER = 1, PC_RESET = 2, PC_RUN = 4;
static const uint32_t IRQ_XFER = 1, IRQ_PORT = 2;

static const int SPEED_FS = 1;
static const int CPB_FS = 5;              // 60 MHz clocks per FS bit
static const int RESP_DELAY_BT = 8;       // device turnaround, bit times

// USB 2.0: a device must detect a reset after 2.5 us of SE0 — 150
// clocks at the 60 MHz line rate.  The strict model requires it.
static const int RESET_MIN_CLKS = 150;

static Vusbhc_wire_test* dut;
static UsbDeviceSim dev;
static int g_pass = 0, g_fail = 0;

static void expect(const char* name, bool ok) {
    if (ok) {
        g_pass++;
        return;
    }
    printf("FAIL %s\n", name);
    g_fail++;
}

// ── The strict device, evaluated once per USB clock ──────────────────────

static std::vector<uint8_t> g_rx_pkt;
static bool g_rx_active_seen = false;
static std::vector<uint8_t> g_resp;
static size_t g_resp_idx = 0;
static int g_resp_wait = 0;
static bool g_presenting = false;

static bool g_in_default = false;   // reset seen: the device may speak
static int g_se0_run = 0;
static int g_se0_max = 0;           // longest continuous SE0 ever seen
static int g_resets_seen = 0;
static int g_sofs_seen = 0;
static int g_pkts_ignored = 0;      // traffic arriving while deaf

// Inputs settle before the USB rising edge, so a presented byte is
// visible to the posedge that loads it — the seam's o_tx_ready is a
// level (serializer empty), meaningful only for a byte already held
// across an edge.
static void dev_apply() {
    if (g_resp_wait > 0 && --g_resp_wait == 0) {
        g_presenting = true;
        g_resp_idx = 0;
    }
    dut->i_dev_tx_valid = g_presenting;
    if (g_presenting)
        dut->i_dev_tx_data = g_resp[g_resp_idx];
}

// Outputs sampled after the falling edge, mid-cycle.
static void dev_sample() {
    // Reset detection on the device's own line view: SE0 is 2'b00.
    if (dut->o_dev_line_state == 0) {
        g_se0_run++;
        if (g_se0_run > g_se0_max)
            g_se0_max = g_se0_run;
    } else {
        if (g_se0_run >= RESET_MIN_CLKS) {
            g_resets_seen++;
            g_in_default = true;
            dev.bus_reset();
        }
        g_se0_run = 0;
    }

    if (dut->o_dev_rx_valid)
        g_rx_pkt.push_back(dut->o_dev_rx_data);
    if (dut->o_dev_rx_active) {
        g_rx_active_seen = true;
    } else if (g_rx_active_seen) {
        g_rx_active_seen = false;
        if (!g_rx_pkt.empty()) {
            if ((g_rx_pkt[0] & 0xf) == 0x5) {
                g_sofs_seen++;          // frame keeping, not a transaction
            } else if (!g_in_default) {
                g_pkts_ignored++;       // strict: deaf until reset
            } else {
                dev.host_packet(g_rx_pkt);
                if (dev.has_response()) {
                    g_resp = dev.take_response();
                    g_resp_wait = RESP_DELAY_BT * CPB_FS;
                }
            }
        }
        g_rx_pkt.clear();
    }

    if (g_presenting && dut->o_dev_tx_ready) {
        if (++g_resp_idx >= g_resp.size()) {
            g_presenting = false;
            dut->i_dev_tx_valid = 0;
        }
    }
}

// ── Clocking: USB every 2 units, CPU every 5 ─────────────────────────────

static uint64_t g_unit = 0;

static bool step_unit() {
    bool cpu_posedge = false;
    g_unit++;
    if (g_unit % 2 == 0) {
        bool rising = !dut->i_usb_clk;
        if (rising)
            dev_apply();
        dut->i_usb_clk = !dut->i_usb_clk;
        dut->eval();
        if (!rising)
            dev_sample();
    }
    if (g_unit % 5 == 0) {
        bool rising = !dut->i_clk;
        dut->i_clk = !dut->i_clk;
        dut->eval();
        cpu_posedge = rising;
    }
    return cpu_posedge;
}

static void cpu_cycle() {
    while (!step_unit()) { }
}

static void cpu_cycles(int n) {
    for (int i = 0; i < n; i++)
        cpu_cycle();
}

// ── Bus master ───────────────────────────────────────────────────────────

static uint32_t bus_read(uint32_t off) {
    dut->i_addr = off;
    dut->i_re = 1;
    int guard = 0;
    do {
        cpu_cycle();
    } while (dut->o_busy && ++guard < 64);
    uint32_t v = dut->o_rdata;
    dut->i_re = 0;
    cpu_cycle();
    return v;
}

static void bus_write(uint32_t off, uint32_t val) {
    dut->i_addr = off;
    dut->i_wdata = val;
    dut->i_we = 1;
    int guard = 0;
    do {
        cpu_cycle();
    } while (dut->o_busy && ++guard < 64);
    dut->i_we = 0;
    cpu_cycle();
}

static bool wait_irq(uint32_t bit, int cap_cycles) {
    for (int i = 0; i < cap_cycles; i += 16) {
        uint32_t st = bus_read(R_IRQ_STATUS);
        if (st & bit) {
            bus_write(R_IRQ_STATUS, bit);
            return true;
        }
        cpu_cycles(8);
    }
    return false;
}

struct TxnOut {
    bool done = false;
    uint32_t status = 0;
    int result() const { return (int)((status >> 1) & 7); }
    int rxlen() const { return (int)((status >> 8) & 0x7f); }
};

static TxnOut run_txn(int pid, int addr, int endp, int tog, int len,
                      int cap_cycles) {
    TxnOut r;
    bus_write(R_TOKEN, (uint32_t)(pid | (addr << 4) | (endp << 11) |
                                  (tog << 16)));
    bus_write(R_XFER_CTRL, (uint32_t)len | (1u << 16));
    r.done = wait_irq(IRQ_XFER, cap_cycles);
    r.status = bus_read(R_XFER_STATUS);
    return r;
}

int main() {
    dut = new Vusbhc_wire_test;
    dut->i_rst = 1;
    dut->i_usb_rst = 1;
    dut->i_clk = 0;
    dut->i_usb_clk = 0;
    dut->i_speed = SPEED_FS;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_we = 0;
    dut->i_re = 0;
    dut->i_dev_tx_data = 0;
    dut->i_dev_tx_valid = 0;
    dut->i_dev_present = 1;
    cpu_cycles(8);
    dut->i_rst = 0;
    dut->i_usb_rst = 0;

    // ── Connect detection: the idle-J pull-up, debounced ─────────────
    bus_write(R_PORT_CTRL, PC_POWER);
    cpu_cycles(200);
    uint32_t ps = bus_read(R_PORT_STATUS);
    expect("connect detected", (ps & 1) != 0);
    expect("full speed detected", ((ps >> 4) & 3) == 2);

    // ── Strictness control: the device is deaf before any reset ──────
    const uint8_t get_dev_desc[8] = {0x80, 0x06, 0x00, 0x01,
                                     0x00, 0x00, 0x08, 0x00};
    bus_write(R_DATA + 0, 0x01000680u);
    bus_write(R_DATA + 4, 0x00080000u);
    bus_write(R_PORT_CTRL, PC_POWER | PC_RUN);
    TxnOut deaf = run_txn(TOK_SETUP, 0, 0, 0, 8, 20000);
    expect("deaf before reset: timeout", deaf.done &&
           deaf.result() == RES_TIMEOUT);
    expect("deaf before reset: packet ignored", g_pkts_ignored > 0 &&
           g_resets_seen == 0);

    // ── SOF delivery on the wire ─────────────────────────────────────
    // RUN has been on for a while; frame markers must be reaching the
    // device as real packets (a FS device suspends without them).
    cpu_cycles(4000);   // ~2.5 scaled frames
    expect("SOFs reach the wire", g_sofs_seen >= 2);

    // ── The port-reset recipe, as the driver runs it ─────────────────
    bus_write(R_PORT_CTRL, PC_POWER | PC_RUN | PC_RESET);
    cpu_cycles(3200);   // hold: 8000 USB clocks of drive
    bus_write(R_PORT_CTRL, PC_POWER | PC_RUN);
    cpu_cycles(400);

    expect("device saw a spec-valid reset", g_resets_seen == 1);
    expect("reset SE0 continuous", g_se0_max >= RESET_MIN_CLKS);
    ps = bus_read(R_PORT_STATUS);
    expect("port enabled after reset", (ps & 2) != 0);

    // ── Enumeration first contact: SETUP GET_DESCRIPTOR(8) ───────────
    dev.out_response = UsbDeviceSim::RSP_ACK;
    bus_write(R_DATA + 0, 0x01000680u);
    bus_write(R_DATA + 4, 0x00080000u);
    TxnOut setup = run_txn(TOK_SETUP, 0, 0, 0, 8, 40000);
    expect("SETUP acked", setup.done && setup.result() == RES_ACK);
    expect("SETUP payload at device",
           dev.out_payload == std::vector<uint8_t>(get_dev_desc,
                                                   get_dev_desc + 8));

    // ── Data stage: the first 8 descriptor bytes come back ───────────
    const std::vector<uint8_t>& dd = UsbDeviceSim::device_descriptor();
    dev.in_response = UsbDeviceSim::RSP_DATA;
    dev.in_payload = std::vector<uint8_t>(dd.begin(), dd.begin() + 8);
    dev.in_toggle = true;
    TxnOut in = run_txn(TOK_IN, 0, 0, 1, 8, 40000);
    expect("IN data acked", in.done && in.result() == RES_ACK &&
           in.rxlen() == 8);
    uint32_t w0 = bus_read(R_DATA + 0), w1 = bus_read(R_DATA + 4);
    expect("descriptor prefix intact",
           (w0 & 0xff) == dd[0] && ((w0 >> 8) & 0xff) == dd[1] &&
           ((w1 >> 24) & 0xff) == dd[7]);

    // ── Status stage: zero-length OUT ────────────────────────────────
    dev.out_response = UsbDeviceSim::RSP_ACK;
    TxnOut st = run_txn(TOK_OUT, 0, 0, 1, 0, 40000);
    expect("status ZLP acked", st.done && st.result() == RES_ACK);

    // ── Back-to-back transactions across frame ticks ─────────────────
    // Each transaction relaunches the moment the previous completes, so
    // the launch point sweeps across the frame marker's period.  Every
    // relative phase must produce a clean transaction: a launch landing
    // inside the previous packet's wire drain, or against a due marker,
    // must wait its turn rather than fuse onto the live serializer.
    // The one-byte host ACK closing an IN transaction owes the wire the
    // most after its seam-level completion, so IN is the stressor.
    int b2b_ok = 0;
    int sofs_before = g_sofs_seen;
    const int B2B_ROUNDS = 24;
    for (int i = 0; i < B2B_ROUNDS; i++) {
        dev.in_response = UsbDeviceSim::RSP_DATA;
        dev.in_payload = {0x5a, (uint8_t)i};
        dev.in_toggle = (i & 1) != 0;
        TxnOut r = run_txn(TOK_IN, 0, 0, (i & 1), 2, 40000);
        if (r.done && r.result() == RES_ACK && r.rxlen() == 2)
            b2b_ok++;
    }
    expect("back-to-back txns all clean", b2b_ok == B2B_ROUNDS);
    expect("SOFs kept flowing through the burst",
           g_sofs_seen - sofs_before >= 2);

    // ── Device dies mid-session, then detach: both must be seen ──────
    // A device whose firmware stops responding leaves failing
    // transactions behind; the port must still notice the physical
    // disconnect afterward — the real-world failure sequence.
    g_in_default = false;              // firmware death: total silence
    TxnOut dead = run_txn(TOK_IN, 0, 0, 0, 8, 40000);
    expect("dead device times out", dead.done &&
           dead.result() == RES_TIMEOUT);

    // ── Detach and replug: the port must notice both ─────────────────
    // An unplug at an enabled, powered port is a disconnect the port
    // controller must debounce and report as a PORT_CHANGE; a replug
    // must be detected the same way a first connect was.  The status
    // view alone is not the contract — an interrupt-driven host only
    // ever learns of the transition through IRQ_STATUS — so each leg
    // acks the sticky bit first and demands a freshly latched event.
    bus_write(R_IRQ_STATUS, IRQ_PORT);
    uint32_t is = bus_read(R_IRQ_STATUS);
    expect("port event quiescent before unplug", (is & IRQ_PORT) == 0);

    dut->i_dev_present = 0;
    cpu_cycles(400);
    ps = bus_read(R_PORT_STATUS);
    expect("unplug drops connect", (ps & 1) == 0);
    is = bus_read(R_IRQ_STATUS);
    expect("unplug latches PORT_CHANGE", (is & IRQ_PORT) != 0);
    bus_write(R_IRQ_ENABLE, IRQ_PORT);
    expect("o_irq follows the latched event", dut->o_irq != 0);
    bus_write(R_IRQ_ENABLE, 0);
    bus_write(R_IRQ_STATUS, IRQ_PORT);

    dut->i_dev_present = 1;
    cpu_cycles(400);
    ps = bus_read(R_PORT_STATUS);
    expect("replug detected", (ps & 1) != 0);
    is = bus_read(R_IRQ_STATUS);
    expect("replug latches PORT_CHANGE", (is & IRQ_PORT) != 0);

    printf("usbhc_wire: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

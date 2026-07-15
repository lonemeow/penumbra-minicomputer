// Verilator testbench for the complete CLASS_USBHC device
//
// Drives the register contract (doc/system/devices/usb-host.md) from the
// bus side, exactly as ROM or kernel software will: CAP discovery, the
// port recipes through PORT_CTRL/PORT_STATUS, DATA window traffic, and
// transactions launched by XFER_CTRL and reaped through IRQ_STATUS +
// XFER_STATUS. UsbDeviceSim answers below through usb_phy_sim's credit
// port. The CPU and USB clocks run at a 5:2 unit ratio (2.5x, the shape
// of the real 25 MHz / 60 MHz split), so every scenario crosses the CDC
// with sliding phase alignment.
//
// This bench is also the executable specification for the ISS's
// register-level USBHC model: any behavior pinned here must hold there.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vusbhc_dev_test.h"
#include "usb_device_sim.h"

// Register offsets (penumbra_pkg USBHC_REG_*).
static const uint32_t R_CAP = 0x00, R_IRQ_STATUS = 0x04, R_IRQ_ENABLE = 0x08,
                      R_PORT_STATUS = 0x0C, R_PORT_CTRL = 0x10,
                      R_FRAME = 0x14, R_TOKEN = 0x18, R_XFER_CTRL = 0x1C,
                      R_XFER_STATUS = 0x20, R_DATA = 0x40;

// Field encodings from the programmer contract.
static const int TOK_SETUP = 0, TOK_OUT = 1, TOK_IN = 2;
static const int RES_ACK = 0, RES_NAK = 1, RES_TIMEOUT = 3;
static const uint32_t PC_POWER = 1, PC_RESET = 2, PC_RUN = 4;
static const uint32_t IRQ_XFER = 1, IRQ_PORT = 2, IRQ_SOF = 4;

static const int SPEED_FS = 1, SPEED_LS = 2;   // usb_speed_e (device port)

// Mirrors the wrapper's parameter overrides (in USB clocks).
static const int CLKS_PER_MS = 4000;

static Vusbhc_dev_test* dut;
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

// ── Device bridge on the credit port (USB clock) ─────────────────────────

static std::vector<uint8_t> g_host_pkt;
static std::vector<uint8_t> g_resp;
static size_t g_resp_idx = 0;
static int g_resp_wait = 0;
static bool g_presenting = false;
static int g_cpb = 5;   // clocks per bit at the attached speed
static const int RESP_DELAY_BT = 8;

// Applied at the start of a USB cycle (just after the rising edge).
static void bridge_apply() {
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

// Sampled mid-cycle (after the falling edge): the cycle's pulses.
static void bridge_sample() {
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
    if (dut->o_keepalive)
        dev.host_packet({0xa5});
    if (dut->o_rx_ready) {
        g_resp_idx++;
        if (g_resp_idx >= g_resp.size())
            g_presenting = false;
    }
}

// ── Two-clock unit stepping: CPU half-period 5, USB half-period 2 ────────

static long g_unit = 0;
static bool step_unit() {
    bool cpu_posedge = false;
    g_unit++;
    if (g_unit % 2 == 0) {
        bool rising = !dut->i_usb_clk;
        dut->i_usb_clk = !dut->i_usb_clk;
        dut->eval();
        if (rising) {
            bridge_apply();
            dut->eval();
        } else {
            bridge_sample();
        }
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

// ── Bus master (the software's view) ─────────────────────────────────────

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

// Polls IRQ_STATUS for a source bit, W1C-clearing it when seen.
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
    int rxtoggle() const { return (int)((status >> 4) & 1); }
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

static void expect_result(const char* name, const TxnOut& r, int want) {
    if (r.done && r.result() == want) {
        g_pass++;
        return;
    }
    printf("FAIL %s: done=%d result=%d want=%d\n", name, r.done, r.result(),
           want);
    g_fail++;
}

int main() {
    dut = new Vusbhc_dev_test;
    dut->i_rst = 1;
    dut->i_usb_rst = 1;
    dut->i_clk = 0;
    dut->i_usb_clk = 0;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_we = 0;
    dut->i_re = 0;
    dut->i_rx_valid = 0;
    dut->i_rx_data = 0;
    dut->i_rx_last = 0;
    dut->i_dev_connect = 0;
    dut->i_dev_speed = SPEED_FS;
    cpu_cycles(4);
    dut->i_rst = 0;
    dut->i_usb_rst = 0;
    cpu_cycles(4);

    // ── CAP and IRQ_ENABLE ───────────────────────────────────────────
    uint32_t cap = bus_read(R_CAP);
    expect("cap fields", (cap & 0xff) == 1 && ((cap >> 8) & 0xff) == 64 &&
           (cap & (1u << 16)) && (cap & (1u << 17)));
    bus_write(R_IRQ_ENABLE, IRQ_XFER | IRQ_PORT);
    expect("irq_enable readback", bus_read(R_IRQ_ENABLE) ==
           (IRQ_XFER | IRQ_PORT));

    // ── DATA window round trip (the byte sequencer, both ways) ───────
    for (int w = 0; w < 16; w++)
        bus_write(R_DATA + 4 * w, 0xA0B1C200u + (uint32_t)w * 0x01010101u);
    bool data_ok = true;
    for (int w = 0; w < 16; w++)
        data_ok &= (bus_read(R_DATA + 4 * w) ==
                    0xA0B1C200u + (uint32_t)w * 0x01010101u);
    expect("data window round trip", data_ok);

    // ── Attach: power on, device pull-up, PORT_CHANGE ────────────────
    bus_write(R_PORT_CTRL, PC_POWER);
    dut->i_dev_connect = 1;
    expect("attach port-change irq", wait_irq(IRQ_PORT, 4000));
    uint32_t ps = bus_read(R_PORT_STATUS);
    expect("attach status", (ps & 1) && ((ps >> 4) & 3) == 2);  // full speed
    expect("port irq line", dut->o_irq == 0);   // W1C already cleared it

    // ── Bus reset recipe ─────────────────────────────────────────────
    bus_write(R_PORT_CTRL, PC_POWER | PC_RESET);
    cpu_cycles(20);
    expect("reset active + at device", (bus_read(R_PORT_STATUS) & 4) &&
           dut->o_bus_reset);
    bus_write(R_PORT_CTRL, PC_POWER);
    expect("reset-complete port-change", wait_irq(IRQ_PORT, 4000));
    expect("port enabled", (bus_read(R_PORT_STATUS) & 2) != 0);

    // ── Control traffic per the transaction model ────────────────────
    std::vector<uint8_t> setup_pl{0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                                  0x40, 0x00};
    bus_write(R_DATA + 0, 0x01000680u);   // little-endian words
    bus_write(R_DATA + 4, 0x00400000u);
    dev.out_response = UsbDeviceSim::RSP_ACK;
    expect_result("setup-ack", run_txn(TOK_SETUP, 0, 0, 0, 8, 8000),
                  RES_ACK);
    expect("setup payload at device", dev.out_payload == setup_pl);

    std::vector<uint8_t> in_pl{0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00,
                               0x08};
    dev.in_response = UsbDeviceSim::RSP_DATA;
    dev.in_payload = in_pl;
    dev.in_toggle = true;
    TxnOut in_r = run_txn(TOK_IN, 0, 0, 0, 8, 8000);
    expect_result("in-data", in_r, RES_ACK);
    expect("in-data length+toggle", in_r.rxlen() == 8 &&
           in_r.rxtoggle() == 1);
    expect("in-data buffer", bus_read(R_DATA + 0) == 0x01100112u &&
           bus_read(R_DATA + 4) == 0x08000000u);

    dev.in_response = UsbDeviceSim::RSP_NAK;
    expect_result("in-nak", run_txn(TOK_IN, 0, 0, 0, 8, 8000), RES_NAK);

    dev.out_response = UsbDeviceSim::RSP_SILENT;
    expect_result("out-timeout", run_txn(TOK_OUT, 0, 1, 1, 8, 8000),
                  RES_TIMEOUT);
    dev.out_response = UsbDeviceSim::RSP_ACK;

    // The interrupt line follows the masked OR: XFER_DONE is enabled, so
    // a completed transaction raises it until the W1C (inside wait_irq).
    bus_write(R_IRQ_STATUS, IRQ_XFER | IRQ_PORT | IRQ_SOF);
    dev.in_response = UsbDeviceSim::RSP_NAK;
    bus_write(R_TOKEN, TOK_IN);
    bus_write(R_XFER_CTRL, 8u | (1u << 16));
    bool irq_rose = false;
    for (int i = 0; i < 8000 && !irq_rose; i++) {
        cpu_cycle();
        irq_rose = dut->o_irq;
    }
    expect("irq line on xfer-done", irq_rose);
    bus_write(R_IRQ_STATUS, IRQ_XFER);
    cpu_cycles(4);
    expect("irq line clears on w1c", dut->o_irq == 0);

    // ── Frame counter and SOF across the CDC ─────────────────────────
    dev.sof_frames.clear();
    uint32_t frame0 = bus_read(R_FRAME) & 0x7ff;
    bus_write(R_PORT_CTRL, PC_POWER | PC_RUN);
    expect("sof irq", wait_irq(IRQ_SOF, CLKS_PER_MS * 2));
    cpu_cycles(CLKS_PER_MS);   // ~2.5 more frames at the 2.5x ratio
    uint32_t frame1 = bus_read(R_FRAME) & 0x7ff;
    bus_write(R_PORT_CTRL, PC_POWER);
    expect("frame advances", ((frame1 - frame0) & 0x7ff) >= 2 &&
           dev.sof_frames.size() >= 2);

    expect("no wire corruption", dev.crc_failures == 0 &&
           dev.pid_failures == 0);

    printf("usbhc_dev: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}

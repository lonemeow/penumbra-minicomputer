// Verilator testbench for the Ethernet adapter, whole
//
// The adapter with a loopback at its seam, driven from the bus alone, as
// a driver drives it (doc/system/devices/ethernet.md, "Driver Flow"):
// write a frame into a transmit slot, count it in TX_PROD, and find it in
// the receive ring. Each module has a testbench of its own. This one is
// for what only the assembly can show: that a frame written through the
// transmit aperture comes back byte for byte through the receive
// aperture, across two clock crossings and two memories, and that the
// counts, the filter settings, the interrupt and a reset all reach the
// far side and back. It runs at several ratios of the bus clock to the
// PHY's.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include "Veth_loop_test.h"

namespace {

Veth_loop_test* dut;
int pass = 0, fail = 0;
long now = 0;                       // time, in units a clock period counts

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 30) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 200) {
        printf("eth_loop: gave up after %d failures\n", fail);
        exit(1);
    }
}

// The adapter's defaults.
constexpr int RX_SLOTS   = 32;
constexpr int TX_SLOTS   = 8;
constexpr int SLOT_BYTES = 2048;
constexpr int MIN_FRAME  = 60;

enum : uint32_t {
    CAP = 0x00, IRQ_STATUS = 0x04, IRQ_ENABLE = 0x08, CTRL = 0x0c, MDIO = 0x14,
    ADDR_LO = 0x18, ADDR_HI = 0x1c, RX_GEOM = 0x40, RX_PROD = 0x44, RX_CONS = 0x48,
    RX_DROP = 0x4c, TX_GEOM = 0x50, TX_PROD = 0x54, TX_CONS = 0x58, TX_WM = 0x5c,
    TX_APERTURE = 0x8000, RX_APERTURE = 0x10000,
};
constexpr uint32_t CTRL_RX_EN = 1, CTRL_TX_EN = 2, CTRL_PROMISC = 4, CTRL_ALLMULTI = 8;
constexpr uint32_t IRQ_RX = 1, IRQ_TX = 2;
constexpr uint32_t BASE = 0xff0a0000u;

typedef std::vector<uint8_t> Bytes;

const uint8_t STATION[6]   = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t GROUP[6]     = {0x01, 0x00, 0x5e, 0x12, 0x34, 0x56};
const uint8_t OTHER[6]     = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};

uint32_t lcg = 0x1234567u;
uint32_t next_random() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

Bytes make_frame(const uint8_t* dest, int len) {
    Bytes f(dest, dest + 6);
    while ((int)f.size() < len) f.push_back((uint8_t)next_random());
    f.resize(len);
    return f;
}

// ── The clocks ───────────────────────────────────────────────
long bus_period = 27, phy_period = 8;
long next_bus = 1, next_phy = 2;
long mdc_rises = 0;
bool mdc_was = false;

// Run to the next edge of the bus clock, with whatever edges of the PHY
// clock come before it.
void bus_tick() {
    while (next_phy <= next_bus) {
        now = next_phy;
        dut->i_phy_clk = 1;
        dut->eval();
        dut->i_phy_clk = 0;
        dut->eval();
        next_phy += phy_period;
    }
    now = next_bus;
    dut->i_clk = 1;
    dut->eval();
    dut->i_clk = 0;
    dut->eval();
    next_bus += bus_period;
    if (dut->o_mdc && !mdc_was) mdc_rises++;
    mdc_was = dut->o_mdc;
}

void run(int bus_cycles) { for (int i = 0; i < bus_cycles; i++) bus_tick(); }

// ── The bus ──────────────────────────────────────────────────
uint32_t access(bool write, uint32_t offset, uint32_t data, int byte_en = 0xf) {
    dut->i_addr    = BASE + offset;
    dut->i_wdata   = data;
    dut->i_byte_en = byte_en;
    dut->i_we      = write;
    dut->i_re      = !write;
    bus_tick();
    check(dut->o_ack, "%s of %05x: no ack the cycle after", write ? "write" : "read", offset);
    uint32_t rdata = dut->o_rdata;
    bus_tick();
    dut->i_we = 0;
    dut->i_re = 0;
    return rdata;
}

uint32_t rd(uint32_t offset) { return access(false, offset, 0); }
void     wr(uint32_t offset, uint32_t data) { access(true, offset, data); }

// ── A driver's side of the rings ─────────────────────────────
uint16_t tx_prod = 0, rx_cons = 0;

// Wait on a register until it reads a value, a bounded while.
bool wait_for(uint32_t offset, uint32_t want, int bus_cycles) {
    for (int i = 0; i < bus_cycles; i += 4) {
        if ((rd(offset) & 0xffffu) == want) return true;
        run(2);
    }
    return false;
}

// Queue one frame: its bytes into the slot at TX_PROD, then LEN, then the
// count. The slot is written a word at a time, and its last few bytes one
// lane at a time where `by_bytes` asks, as a driver stores an odd end.
void queue(const Bytes& frame, bool by_bytes = false) {
    check((uint16_t)(tx_prod - (uint16_t)rd(TX_CONS)) < TX_SLOTS,
          "queueing into a full transmit ring");
    uint32_t slot = TX_APERTURE + (tx_prod % TX_SLOTS) * SLOT_BYTES;
    Bytes image = {0, 0};
    image.insert(image.end(), frame.begin(), frame.end());
    size_t whole = by_bytes && image.size() > 8 ? (image.size() - 5) & ~3u : image.size();
    for (size_t at = 4; at < whole; at += 4) {
        uint32_t w = 0;
        for (int i = 0; i < 4 && at + i < image.size(); i++)
            w |= (uint32_t)image[at + i] << (8 * i);
        wr(slot + at, w);
    }
    for (size_t at = whole < 4 ? 4 : whole; at < image.size(); at++)
        access(true, slot + (at & ~3u), (uint32_t)image[at] << (8 * (at & 3)), 1 << (at & 3));
    // The first word last: LEN and the frame's first two bytes.
    uint32_t first = (uint32_t)frame.size();
    for (int i = 2; i < 4 && i < (int)image.size(); i++) first |= (uint32_t)image[i] << (8 * i);
    wr(slot, first);
    tx_prod++;
    wr(TX_PROD, tx_prod);
}

// Read the oldest stored frame out of the receive ring and release it.
Bytes take() {
    uint32_t slot = RX_APERTURE + (rx_cons % RX_SLOTS) * SLOT_BYTES;
    uint32_t first = rd(slot);
    int len = first & 0xffff;
    Bytes frame;
    check(len >= MIN_FRAME && len <= 1518, "LEN %d in a received slot", len);
    if (len > 1518) len = 1518;
    uint32_t word = first;
    for (int at = 2; at < len + 2; at++) {
        if ((at & 3) == 0) word = rd(slot + at);
        frame.push_back((uint8_t)(word >> (8 * (at & 3))));
    }
    rx_cons++;
    wr(RX_CONS, rx_cons);
    return frame;
}

// How long a frame of this length takes to go round, in bus cycles, with
// room to spare: every byte time is two PHY cycles.
int round_trip(size_t len) {
    return (int)((len + 100) * 2 * phy_period / bus_period) + 400;
}

// Bus cycles for a change on one side to have reached the other and come
// back, with room to spare: a few cycles of each clock.
int settle() { return 300 + (int)(30 * phy_period / bus_period); }

// Send a frame and see what comes of it: the frame back, padded if it was
// short, or nothing.
void loop(const Bytes& frame, bool expect_back, const char* what, bool by_bytes = false) {
    queue(frame, by_bytes);
    check(wait_for(TX_CONS, tx_prod, round_trip(frame.size())),
          "%s, %zu bytes: TX_CONS never counted it", what, frame.size());
    bool back = wait_for(RX_PROD, (uint16_t)(rx_cons + 1), settle());
    check(back == expect_back, "%s, %zu bytes: %s", what, frame.size(),
          back ? "received, and it should not have been" : "never received");
    if (!back) return;
    Bytes want = frame;
    while (want.size() < MIN_FRAME) want.push_back(0);
    Bytes got = take();
    check(got.size() == want.size(), "%s: %zu bytes received for %zu sent", what, got.size(),
          frame.size());
    size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at]) at++;
    check(at == want.size() || at == got.size(),
          "%s, %zu bytes: byte %zu came back %02x, sent %02x", what, frame.size(), at,
          at < got.size() ? got[at] : 0, at < want.size() ? want[at] : 0);
}

// The machine's reset, far longer than a crossing takes, and the device
// brought up as a driver brings it up.
void reset_and_bring_up(uint32_t ctrl) {
    dut->i_rst = 1;
    dut->i_we = dut->i_re = 0;
    run(600);
    dut->i_rst = 0;
    run(settle());
    tx_prod = rx_cons = 0;
    wr(ADDR_LO, 0x33221102u);
    wr(ADDR_HI, 0x00005544u);
    wr(CTRL, ctrl);
    run(settle());
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_loop_test;
    dut->i_clk = 0;
    dut->i_phy_clk = 0;
    dut->i_rst = 0;
    dut->i_we = dut->i_re = 0;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_byte_en = 0;
    dut->i_factory_addr = 0x0a0b0c0d0e0fULL;
    dut->i_factory_valid = 1;
    dut->i_mdio = 1;                // the pull-up, and no PHY on it
    dut->eval();

    // (bus period, PHY period): a bus slower than the PHY's clock, one
    // faster, and one much faster, as against a 10 Mbit/s link.
    const long clocks[][2] = {{27, 8}, {17, 40}, {17, 400}};

    for (const auto& c : clocks) {
        bus_period = c[0];
        phy_period = c[1];
        next_bus = now + 1;
        next_phy = now + 2;
        bool slow = phy_period > 100;       // fewer and shorter frames where a byte takes long

        reset_and_bring_up(CTRL_RX_EN | CTRL_TX_EN);

        // What the device says of itself: version 1, a managed PHY, a
        // station address, and the loopback's three speeds.
        {
            uint32_t cap = rd(CAP);
            check(cap == 0x00070301u, "CAP reads %08x", cap);
            check(rd(RX_GEOM) == (((uint32_t)SLOT_BYTES << 16) | RX_SLOTS) &&
                  rd(TX_GEOM) == (((uint32_t)SLOT_BYTES << 16) | TX_SLOTS),
                  "ring geometry reads %08x, %08x", rd(RX_GEOM), rd(TX_GEOM));
        }

        // A frame round, at every alignment of its end and both extremes
        // of length, and one too short to send unpadded.
        for (int len : {60, 61, 62, 63, 64, 65, 100, 1514}) {
            if (slow && len > 100) continue;
            loop(make_frame(STATION, len), true, "to the station");
        }
        loop(make_frame(STATION, 20), true, "short, to be padded");
        loop(make_frame(BROADCAST, 77), true, "broadcast");
        loop(make_frame(STATION, 93), true, "stored a byte lane at a time", true);

        // The filter settings reach the receiver.
        loop(make_frame(OTHER, 64), false, "to another station");
        loop(make_frame(GROUP, 64), false, "to a group, ALLMULTI clear");
        wr(CTRL, CTRL_RX_EN | CTRL_TX_EN | CTRL_ALLMULTI);
        run(settle());
        loop(make_frame(GROUP, 64), true, "to a group, ALLMULTI set");
        loop(make_frame(OTHER, 64), false, "to another station, ALLMULTI set");
        wr(CTRL, CTRL_RX_EN | CTRL_TX_EN | CTRL_PROMISC);
        run(settle());
        loop(make_frame(OTHER, 64), true, "to another station, PROMISC set");
        wr(CTRL, CTRL_RX_EN | CTRL_TX_EN);
        run(settle());

        // The speed software writes reaches both sides of the seam.
        wr(CTRL, CTRL_RX_EN | CTRL_TX_EN | (1u << 8));
        run(settle());
        check(dut->o_rx_speed == 1 && dut->o_tx_speed == 1, "CTRL.SPEED 1 at the seam: %d, %d",
              dut->o_rx_speed, dut->o_tx_speed);
        wr(CTRL, CTRL_RX_EN | CTRL_TX_EN);
        run(settle());

        // The interrupt: RX while a stored frame is unreleased, TX while
        // the transmit ring is below its watermark.
        {
            wr(IRQ_ENABLE, IRQ_RX);
            check(!dut->o_irq, "interrupt with the receive ring empty");
            Bytes f = make_frame(STATION, 64);
            queue(f);
            check(wait_for(RX_PROD, (uint16_t)(rx_cons + 1), round_trip(64) + settle()),
                  "the frame for the interrupt never arrived");
            check(dut->o_irq && (rd(IRQ_STATUS) & IRQ_RX), "no RX interrupt with a frame stored");
            take();
            check(!dut->o_irq && !(rd(IRQ_STATUS) & IRQ_RX),
                  "RX interrupt still up with the frame released");

            wr(IRQ_ENABLE, IRQ_TX);
            check(!dut->o_irq, "TX interrupt with TX_WM zero");
            wr(TX_WM, 1);
            check(dut->o_irq, "no TX interrupt with nothing queued and TX_WM 1");
            wr(TX_WM, 0);
            wr(IRQ_ENABLE, 0);
        }

        // A full receive ring: frames past it are lost and counted, and
        // the ring holds exactly its slots.
        {
            int extra = 3;
            uint32_t drops_before = rd(RX_DROP);
            for (int i = 0; i < RX_SLOTS + extra; i++) {
                queue(make_frame(STATION, 60 + i));
                check(wait_for(TX_CONS, tx_prod, round_trip(100)), "frame %d never sent", i);
            }
            run(2 * settle());
            uint16_t held = (uint16_t)(rd(RX_PROD) - rx_cons);
            uint32_t drops = rd(RX_DROP) - drops_before;
            check(held == RX_SLOTS, "%u frames held by a ring of %d", held, RX_SLOTS);
            check(drops == (uint32_t)extra, "RX_DROP counted %u of %d frames lost", drops, extra);
            for (int i = 0; i < RX_SLOTS; i++) {
                Bytes got = take();
                check((int)got.size() == 60 + i, "frame %d out of the full ring is %zu bytes", i,
                      got.size());
            }
        }

        // TX_EN cleared: what is queued is discarded, unsent.
        {
            wr(CTRL, CTRL_RX_EN);
            run(settle());
            for (int i = 0; i < 4; i++) queue(make_frame(STATION, 64));
            check(wait_for(TX_CONS, tx_prod, 4 * settle()),
                  "TX_CONS never reached TX_PROD with TX_EN clear");
            run(round_trip(64));
            check((uint16_t)rd(RX_PROD) == rx_cons, "a frame queued with TX_EN clear was sent");
            wr(CTRL, CTRL_RX_EN | CTRL_TX_EN);
            run(settle());
            loop(make_frame(STATION, 70), true, "after TX_EN set again");
        }

        // A management transaction reaches the pins: 64 clocks, and with
        // no PHY to answer, all ones.
        {
            long rises = mdc_rises;
            wr(MDIO, (1u << 24) | (2u << 16));
            check(rd(MDIO) >> 31, "MDIO not BUSY after a write");
            run(900);
            uint32_t v = rd(MDIO);
            check(v == ((1u << 24) | (2u << 16) | 0xffffu), "MDIO reads %08x with no PHY", v);
            check(mdc_rises - rises == 64, "MDC rose %ld times in a frame", mdc_rises - rises);
        }

        // A reset with frames in flight and in the rings: both counts
        // back to zero on both sides, and the device as good as new.
        {
            for (int i = 0; i < 3; i++) queue(make_frame(STATION, 200));
            run(50);
            reset_and_bring_up(CTRL_RX_EN | CTRL_TX_EN);
            check(rd(RX_PROD) == 0 && rd(TX_CONS) == 0 && rd(TX_PROD) == 0 && rd(RX_CONS) == 0,
                  "counts after reset: %u %u %u %u", rd(RX_PROD), rd(RX_CONS), rd(TX_PROD),
                  rd(TX_CONS));
            run(round_trip(200));
            check(rd(RX_PROD) == 0, "a frame from before the reset arrived after it");
            loop(make_frame(STATION, 66), true, "after a reset");
        }
    }

    printf("eth_loop: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

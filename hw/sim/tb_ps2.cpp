// Verilator testbench for the PS/2 port
//
// The port is reached as software reaches it, through its registers
// (doc/system/devices/ps2.md), with a device modelled on the two lines
// (ps2_device.h). The lines are wired-AND: each is low while the port
// or the device pulls it. The checks are the interface's promises: a
// port not in use holds the device off, a received byte arrives whole
// and flagged when its frame is malformed, a byte waiting holds the
// device off so that none is lost, a byte sent reaches the device and
// its failure is reported inside 20 ms, and sending goes ahead of
// receiving without disturbing a byte that waits.

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <vector>
#include "Vps2_test.h"
#include "ps2_device.h"

namespace {

Vps2_test* dut;
Ps2Device  dev;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 30) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
}

// The rate of the port's clock, as the harness reports it (o_clk_hz).
// The port keeps its times in cycles of it, and so does everything
// here: a time is written in ns or us and turned into cycles.
long clk_hz = 0;
long ns(long n) { return n * clk_hz / 1'000'000'000; }
long us(long n) { return n * clk_hz / 1'000'000; }
long to_us(long cycles) { return cycles * 1'000'000 / clk_hz; }

// The port's filter in cycles: ps2_link's FILTER_NS.
long filter_cycles() { return ns(1000); }

// The registers (doc/system/devices/ps2.md).
enum : uint32_t {
    CAP = 0x00, IRQ_STATUS = 0x04, IRQ_ENABLE = 0x08, CTRL = 0x0C,
    RX_DATA = 0x10, TX_DATA = 0x14
};
constexpr uint32_t IRQ_RX = 1, IRQ_TX = 2;
constexpr uint32_t RX_VALID = 0x100, RX_ERR = 0x200;
constexpr uint32_t TX_BUSY = 0x100, TX_ERR = 0x200;

// The interface's bound on a transmission, and what a device is owed
// ahead of a request to send.
long t_tx_bound() { return us(20000); }
long t_claim()    { return us(100); }

// ── The lines ────────────────────────────────────────────────
// A pulse the testbench puts on the clock line at the port's pin, which
// the device does not see: noise, for the port's filter.
long noise_until = 0;

// What the port asks of the lines, watched for the protocol's rules.
struct Monitor {
    bool prev_clk = false, prev_data = true;
    long clk_low = 0;               // cycles the port has now pulled the clock low
    long data_low = 0;              // and the data line
    long data_low_total = 0;
    int  requests = 0;              // times the port let the clock go under data held low
    long last_claim = 0;            // the clock's low time ahead of the last of them
    long last_request = 0;          // and the data line's

    void step(bool port_clk, bool port_data, bool clk_line) {
        if (port_clk && !prev_clk && !port_data) {
            requests++;
            last_claim = clk_low;
            last_request = data_low;
        }
        // The device takes each bit as the clock rises, so the port
        // moves the data line only under a low clock.
        if (port_data != prev_data && dev.clocking_in())
            check(!clk_line, "the port moved data under a high clock at %ld", now);

        clk_low  = port_clk  ? 0 : clk_low + 1;
        data_low = port_data ? 0 : data_low + 1;
        if (!port_data) data_low_total++;
        prev_clk = port_clk;
        prev_data = port_data;
    }
} mon;

// The cycles in 960 ms, about three times what a run takes. A wait
// that never ends, on a port that has stopped answering, fails the run
// here rather than hanging it.
long run_limit = 0;

void tick() {
    if (now >= run_limit) {
        printf("  FAIL [the run passed %ld cycles: something never answered]\n", run_limit);
        printf("%d checks passed, %d failed\n", pass, fail + 1);
        exit(1);
    }

    bool clk_line  = dut->o_ps2_clk && dev.clk;
    bool data_line = dut->o_ps2_data && dev.data;
    dut->i_ps2_clk  = clk_line && now >= noise_until;
    dut->i_ps2_data = data_line;
    mon.step(dut->o_ps2_clk, dut->o_ps2_data, clk_line);

    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
    now++;

    dev.step(clk_line, data_line);
}

void idle(long n) { for (long i = 0; i < n; i++) tick(); }

// ── The bus ──────────────────────────────────────────────────
// An access as the bus makes one: the strobe held through the ack
// cycle, the device acting in the first.
uint32_t rd(uint32_t off) {
    dut->i_addr = 0x4000 | off;
    dut->i_re = 1;
    tick();
    check(dut->o_ack, "no ack for a read of %02x", off);
    uint32_t v = dut->o_rdata;
    tick();
    dut->i_re = 0;
    return v;
}

void wr(uint32_t off, uint32_t v) {
    dut->i_addr = 0x4000 | off;
    dut->i_wdata = v;
    dut->i_we = 1;
    tick();
    check(dut->o_ack, "no ack for a write of %02x", off);
    tick();
    dut->i_we = 0;
}

// ── Software's flows ─────────────────────────────────────────
// Wait for a byte without taking it.
bool wait_rx(long limit = us(8000)) {
    for (long t0 = now; now - t0 < limit; idle(50))
        if (rd(IRQ_STATUS) & IRQ_RX) return true;
    return false;
}

// Take the next byte: RX_DATA as read, or 0 if none arrives.
uint32_t take(long limit = us(8000)) {
    return wait_rx(limit) ? rd(RX_DATA) : 0;
}

// Send a byte and wait for the end: TX_DATA as read once BUSY is clear.
uint32_t send(uint8_t byte) {
    long t0 = now;
    wr(TX_DATA, byte);
    uint32_t v = rd(TX_DATA);
    check(v & TX_BUSY, "send %02x: BUSY clear straight after the write", byte);
    while ((v & TX_BUSY) && now - t0 < us(30000)) {
        idle(100);
        v = rd(TX_DATA);
    }
    check(!(v & TX_BUSY), "send %02x: BUSY still set after 30 ms", byte);
    check(now - t0 < t_tx_bound(), "send %02x: BUSY for %ld us, the bound is 20000",
          byte, to_us(now - t0));
    return v;
}

// Wait out a transmission started with a bare write.
void wait_tx(const char* what) {
    long t0 = now;
    while ((rd(TX_DATA) & TX_BUSY) && now - t0 < us(30000)) idle(100);
    check(!(rd(TX_DATA) & TX_BUSY), "%s: BUSY still set after 30 ms", what);
}

// One received byte, one quantity to a check.
void rx_byte_is(const char* what, uint32_t v, uint8_t want) {
    check(v & RX_VALID, "%s: no byte arrived", what);
    check((v & 0xFF) == want, "%s: BYTE %02x, want %02x", what, v & 0xFF, want);
}

void rx_err_is(const char* what, uint32_t v, bool want) {
    check(v & RX_VALID, "%s: no byte arrived", what);
    check(!!(v & RX_ERR) == want, "%s: ERR %d, want %d", what, !!(v & RX_ERR), want);
}

int finish() {
    check(now < run_limit / 2, "the run took %ld cycles, over half its limit", now);
    printf("%d checks passed, %d failed (bus clock %g MHz)\n", pass, fail, clk_hz / 1e6);
    delete dut;
    return fail ? 1 : 0;
}

constexpr uint16_t START_BIT = 1u << 0, PARITY_BIT = 1u << 9, STOP_BIT = 1u << 10;

} // namespace

int main() {
    dut = new Vps2_test;
    dut->i_clk = 0;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_we = 0;
    dut->i_re = 0;
    dut->i_ps2_clk = 1;
    dut->i_ps2_data = 1;
    dut->eval();                    // the outputs as configured, ahead of the first clock

    clk_hz = dut->o_clk_hz;
    run_limit = us(960'000);
    dev.step_hz(clk_hz);

    dut->i_rst = 1;
    idle(4);
    dut->i_rst = 0;

    // ── A port not in use ────────────────────────────────────
    // The device has a byte to send from the start, and is held off.
    dev.send(0x1C);
    idle(us(3000));
    check(rd(CAP) == 1, "CAP after reset: %08x", rd(CAP));
    check(rd(CTRL) == 0, "CTRL after reset");
    check(rd(IRQ_ENABLE) == 0, "IRQ_ENABLE after reset");
    check(rd(IRQ_STATUS) == 0, "IRQ_STATUS of a port not in use: %x", rd(IRQ_STATUS));
    check(rd(RX_DATA) == 0, "RX_DATA of a port not in use");
    check(rd(TX_DATA) == 0, "TX_DATA of a port not in use");
    check(!dut->o_ps2_clk, "a port not in use lets the clock go");
    check(dut->o_ps2_data, "a port not in use pulls data low");
    check(dev.started == 0, "the device sent to a port not in use");
    check(!dut->o_irq, "interrupt from a port not in use");

    wr(TX_DATA, 0xFF);
    idle(us(500));
    check(rd(TX_DATA) == 0, "TX_DATA after a write to a port not in use: %x", rd(TX_DATA));
    check(mon.data_low_total == 0, "a write to a port not in use moved the data line");

    wr(IRQ_ENABLE, 3);
    check(rd(IRQ_ENABLE) == 3, "IRQ_ENABLE reads back %x", rd(IRQ_ENABLE));
    check(!dut->o_irq, "interrupt enabled on a port not in use");
    wr(IRQ_ENABLE, 0);

    wr(CTRL, 1);
    check(rd(CTRL) == 1, "CTRL reads back %x", rd(CTRL));
    check(rd(IRQ_STATUS) & IRQ_TX, "IRQ_STATUS.TX of an idle port in use");

    // ── The receiver's byte and error flag ───────────────────
    // What ps2_link assembles from a frame's bits, a clause to a check.
    {
        uint32_t v = take();
        check(v & RX_VALID, "the byte held in the device while the port was not in use");
        rx_byte_is("byte order", v, 0x1C);
        check(rd(RX_DATA) == 0, "RX_DATA read again after its byte was taken");

        for (uint8_t b : {0x01, 0x80, 0xA6, 0x00, 0xFF, 0x7E}) {
            dev.send(b);
            rx_byte_is("byte order", take(), b);
        }

        // Odd and even counts of ones in the byte, so both values of
        // the parity bit.
        for (uint8_t b : {0x1C, 0x03, 0x00, 0xFF, 0x80}) {
            dev.send(b);
            rx_err_is("a well-formed frame", take(), false);
        }

        dev.send_bits(Ps2Device::frame_of(0x1C) | START_BIT);
        rx_err_is("start bit of 1", take(), true);
        dev.send(0x03);
        rx_err_is("a well-formed frame after a bad start bit", take(), false);

        dev.send_bits(Ps2Device::frame_of(0x1C) & ~STOP_BIT);
        rx_err_is("stop bit of 0", take(), true);
        dev.send(0x03);
        rx_err_is("a well-formed frame after a bad stop bit", take(), false);

        dev.send_bits(Ps2Device::frame_of(0x1C) ^ PARITY_BIT);
        rx_err_is("even parity, the parity bit 1", take(), true);
        dev.send_bits(Ps2Device::frame_of(0x03) ^ PARITY_BIT);
        rx_err_is("even parity, the parity bit 0", take(), true);
        dev.send(0x1C);
        rx_err_is("a well-formed frame after bad parity", take(), false);

        dev.send_bits(Ps2Device::frame_of(0x1C) ^ PARITY_BIT);
        rx_byte_is("the byte of a malformed frame", take(), 0x1C);

        // A frame the device stops partway: start bit, then 1, 1, 1, 0.
        dev.send_bits(0x0E, 5);
        idle(us(3000));
        check(!(rd(IRQ_STATUS) & IRQ_RX), "a frame cut short arrived as a byte");
        dev.send(0x03);
        {
            uint32_t w = take();
            rx_err_is("the frame after one cut short", w, false);
            rx_byte_is("the frame after one cut short", w, 0x03);
        }

        // And one cut short after a start bit of 1.
        dev.send_bits(0x01, 3);
        idle(us(3000));
        dev.send(0x1C);
        rx_err_is("the frame after one cut short on a bad start bit", take(), false);

        check(dev.cut_short == 2, "the device cut %d frames short", dev.cut_short);
    }

    if (fail) {
        printf("The receiver's byte and error flag are not right yet, and the rest of\n"
               "the run reads bytes through them: stopping here.\n");
        return finish();
    }

    // ── Clock rates ──────────────────────────────────────────
    // The fastest clock a device may give and a slow one, the bit on
    // the line the 5 us the protocol allows before the clock falls.
    for (long half : {us(15), us(50)}) {
        dev.half = (int)half;
        dev.setup = (int)us(5);
        for (uint8_t b : {0xA6, 0x03, 0xFF}) {
            dev.send(b);
            uint32_t v = take();
            check(v == (RX_VALID | b), "clock half period %ld us: RX_DATA %03x, want %03x",
                  to_us(half), v, RX_VALID | b);
        }
    }
    dev.half = (int)us(40);

    // ── A byte waiting holds the device off ──────────────────
    {
        int started = dev.started;
        dev.send(0x11);
        dev.send(0x22);
        check(wait_rx(), "first of two bytes");
        idle(us(6000));
        check(dev.started == started + 1, "the device started %d frames past a byte waiting",
              dev.started - started - 1);
        check(!dut->o_ps2_clk, "the clock let go while a byte waits");
        check(rd(RX_DATA) == (RX_VALID | 0x11), "the byte that waited");
        check(take() == (RX_VALID | 0x22), "the byte held in the device meanwhile");
    }

    // A slow reader loses nothing.
    {
        size_t first = dev.sent_log.size();
        for (uint8_t b : {0x10, 0x20, 0x30, 0x40, 0x50, 0x60}) dev.send(b);
        for (uint8_t b : {0x10, 0x20, 0x30, 0x40, 0x50, 0x60}) {
            check(wait_rx(), "byte %02x for a slow reader", b);
            idle(us(3000));
            uint32_t v = rd(RX_DATA);
            check(v == (RX_VALID | b), "slow reader: RX_DATA %03x, want %03x", v, RX_VALID | b);
        }
        check(dev.sent_log.size() == first + 6, "the device counts %zu of 6 sent",
              dev.sent_log.size() - first);
    }

    // Nor does one reading as fast as the bus allows, where the hold
    // lasts a few cycles.
    {
        std::vector<uint32_t> got;
        for (uint8_t b : {0xE0, 0x75, 0xE0, 0xF0, 0x75}) dev.send(b);
        for (long t0 = now; got.size() < 5 && now - t0 < us(20000); ) {
            uint32_t v = rd(RX_DATA);
            if (v & RX_VALID) got.push_back(v);
        }
        const uint32_t want[] = {0xE0, 0x75, 0xE0, 0xF0, 0x75};
        check(got.size() == 5, "a fast reader took %zu of 5 bytes", got.size());
        for (size_t i = 0; i < got.size() && i < 5; i++)
            check(got[i] == (RX_VALID | want[i]), "fast reader, byte %zu: %03x", i, got[i]);
    }
    check(dev.abandoned == 0, "the device gave up %d frames to a hold", dev.abandoned);

    // ── Interrupts ───────────────────────────────────────────
    {
        wr(IRQ_ENABLE, IRQ_RX);
        idle(10);
        check(!dut->o_irq, "RX interrupt with no byte waiting");
        dev.send(0x5A);
        check(wait_rx(), "byte for the RX interrupt");
        check(dut->o_irq, "no RX interrupt with a byte waiting");
        rd(RX_DATA);
        check(!dut->o_irq, "RX interrupt after its byte was taken");

        wr(IRQ_ENABLE, IRQ_TX);
        idle(10);
        check(dut->o_irq, "no TX interrupt from an idle port");
        wr(TX_DATA, 0xEE);
        idle(10);
        check(!dut->o_irq, "TX interrupt during a transmission");
        check(!(rd(IRQ_STATUS) & IRQ_TX), "IRQ_STATUS.TX during a transmission");
        wait_tx("the transmission under the TX interrupt");
        check(dut->o_irq, "no TX interrupt once the transmission ended");

        wr(IRQ_ENABLE, 0);
        idle(10);
        check(!dut->o_irq, "interrupt with nothing enabled");
        check(rd(IRQ_STATUS) & IRQ_TX, "IRQ_STATUS.TX with nothing enabled");
    }

    // ── Sending ──────────────────────────────────────────────
    // The device answers each byte with an acknowledgement code, as a
    // keyboard does.
    dev.on_byte = [](uint8_t) { dev.reply(0xFA); };
    {
        size_t first = dev.received.size();
        for (uint8_t b : {0xED, 0x00, 0xFF, 0xF4, 0x55, 0x01}) {
            uint32_t v = send(b);
            check(!(v & TX_ERR), "send %02x: ERR set", b);
            check(dev.received.size() > first && dev.received.back() == b,
                  "send %02x: the device took %02x", b,
                  dev.received.empty() ? 0 : dev.received.back());
            check(mon.last_claim >= t_claim(), "send %02x: the clock held low %ld us ahead",
                  b, to_us(mon.last_claim));
            check(mon.last_request > 0, "send %02x: data not low before the clock was let go", b);
            check(take() == (RX_VALID | 0xFA), "send %02x: the device's reply", b);
        }
        check(dev.received.size() == first + 6, "the device took %zu of 6 bytes",
              dev.received.size() - first);
        check(dev.bad_parity == 0, "%d bytes reached the device with bad parity", dev.bad_parity);
        check(dev.bad_stop == 0, "%d bytes reached the device with no stop bit", dev.bad_stop);
    }

    // A write while BUSY is ignored.
    {
        size_t first = dev.received.size();
        wr(TX_DATA, 0xF4);
        idle(us(50));
        wr(TX_DATA, 0x00);
        wait_tx("the byte ahead of an ignored write");
        idle(us(3000));
        check(dev.received.size() == first + 1 && dev.received.back() == 0xF4,
              "a write while BUSY: the device took %zu bytes, the last %02x",
              dev.received.size() - first, dev.received.back());
        check(take() == (RX_VALID | 0xFA), "the reply to the byte ahead of an ignored write");
    }

    // Nothing attached: the transmission ends in ERR inside the bound,
    // and the next one that starts clears it.
    {
        dev.present = false;
        uint32_t v = send(0xEE);
        check(v & TX_ERR, "nothing attached: ERR clear");
        check(rd(TX_DATA) & TX_ERR, "ERR did not stay set");
        check(dut->o_ps2_clk && dut->o_ps2_data, "a line held after a transmission gave up");
        dev.present = true;
        idle(us(500));

        wr(TX_DATA, 0xEE);
        check(!(rd(TX_DATA) & TX_ERR), "ERR still set once the next transmission started");
        wait_tx("the byte to a port just filled");
        check(!(rd(TX_DATA) & TX_ERR), "ERR set after a byte the device took");
        check(take() == (RX_VALID | 0xFA), "the reply after an empty port was filled");
    }

    // A device that clocks the byte in and does not acknowledge it.
    {
        dev.acknowledge = false;
        check(send(0xED) & TX_ERR, "no acknowledgement: ERR clear");
        dev.acknowledge = true;
    }

    // A device that stops clocking partway.
    {
        dev.stall_after = 4;
        check(send(0xED) & TX_ERR, "a device that stalled: ERR clear");
        check(dev.stalled == 1, "the device stalled %d times", dev.stalled);
        dev.stall_after = -1;
        idle(us(500));
        check(!(send(0xED) & TX_ERR), "ERR set on the byte after a stalled one");
        check(take() == (RX_VALID | 0xFA), "the reply after a stalled byte");
    }

    // ── Sending goes ahead of receiving ──────────────────────
    // A byte the device has begun to send is given up and sent again.
    {
        int abandoned = dev.abandoned;
        size_t first = dev.received.size();
        dev.send(0x5A);
        for (long t0 = now; !(dev.sending() && dev.bit == 4) && now - t0 < us(8000); ) tick();
        check(dev.sending() && dev.bit == 4, "the device never got four bits into its frame");
        check(!(send(0xED) & TX_ERR), "a send over a frame in flight: ERR set");
        check(dev.abandoned == abandoned + 1, "the device gave up %d frames to the send",
              dev.abandoned - abandoned);
        check(dev.received.size() == first + 1 && dev.received.back() == 0xED,
              "the byte sent over a frame in flight");
        check(take() == (RX_VALID | 0xFA), "the reply, ahead of the byte given up");
        check(take() == (RX_VALID | 0x5A), "the byte given up, sent again");
    }

    // A byte waiting stays as it is, and the reply waits behind it.
    {
        size_t first = dev.received.size();
        size_t sent = dev.sent_log.size();
        dev.send(0x33);
        check(wait_rx(), "a byte to wait through a send");
        check(!(send(0xED) & TX_ERR), "a send with a byte waiting: ERR set");
        check(dev.received.size() == first + 1, "the byte sent with a byte waiting");
        idle(us(4000));
        check(dev.sent_log.size() == sent + 1, "the reply came past a byte waiting");
        check(rd(RX_DATA) == (RX_VALID | 0x33), "the byte that waited through a send");
        check(take() == (RX_VALID | 0xFA), "the reply that waited behind it");
    }
    dev.on_byte = nullptr;

    // ── Clearing CTRL.EN ─────────────────────────────────────
    {
        dev.send(0x44);
        check(wait_rx(), "a byte to discard");
        wr(CTRL, 0);
        check(rd(RX_DATA) == 0, "RX_DATA after CTRL.EN was cleared over a byte");
        check(rd(IRQ_STATUS) == 0, "IRQ_STATUS after CTRL.EN was cleared");
        wr(CTRL, 1);
        idle(us(2000));
        check(rd(RX_DATA) == 0, "the discarded byte came back");

        size_t first = dev.received.size();
        dev.tx_delay = (int)us(5000);
        wr(TX_DATA, 0xED);
        idle(us(1000));
        check(rd(TX_DATA) & TX_BUSY, "a transmission to abandon");
        wr(CTRL, 0);
        check(rd(TX_DATA) == 0, "TX_DATA after CTRL.EN was cleared over a transmission");
        idle(10);
        check(!dut->o_ps2_clk && dut->o_ps2_data, "the lines after a transmission was abandoned");
        idle(us(8000));
        wr(CTRL, 1);
        idle(us(2000));
        check(dev.received.size() == first, "the device took an abandoned byte");
        check(rd(TX_DATA) == 0, "TX_DATA once the port was in use again");
        dev.tx_delay = (int)us(100);
    }

    // ── Noise ────────────────────────────────────────────────
    // Pulses on the clock shorter than the filter are not clock edges:
    // none starts a frame, and none inside a frame adds a bit to it.
    {
        const long pulse = ns(600);
        for (int i = 0; i < 20; i++) {
            noise_until = now + pulse;
            idle(us(40));
        }
        check(!(rd(IRQ_STATUS) & IRQ_RX), "noise on an idle clock made a byte");
        dev.send(0xA6);
        check(take() == (RX_VALID | 0xA6), "the byte after noise on an idle clock");

        dev.send(0x1C);
        for (long t0 = now, pulses = 0; pulses < 8 && now - t0 < us(8000); ) {
            tick();
            if (dev.st == Ps2Device::SEND_HIGH && dev.count == us(12)) {
                noise_until = now + pulse;
                pulses++;
            }
        }
        check(take() == (RX_VALID | 0x1C), "a byte with noise on its clock's high halves");
    }

    // The port's own pull is never one either, however long it lasts
    // against the filter: CTRL.EN cleared for a cycle count either side
    // of the filter's delay, then a byte at once. The delay is the
    // filter and a few flops, the same few at every clock rate, so the
    // lengths are the filter's own with a fixed margin of cycles each
    // way. The pull starts on a free clock, past the device's last
    // pulse, so that the fall at the pin is the port's alone.
    for (long low = filter_cycles() - 13; low <= filter_cycles() + 19; low++) {
        idle(us(100));
        check(dut->i_ps2_clk, "the clock was not free ahead of clearing CTRL.EN");
        wr(CTRL, 0);
        idle(low);
        wr(CTRL, 1);
        dev.send(0x1C);
        uint32_t v = take();
        check(v == (RX_VALID | 0x1C), "CTRL.EN clear for %ld cycles: RX_DATA %03x", low + 2, v);
    }

    // ── Everything at once ───────────────────────────────────
    // Bytes both ways at random, read at random delays: every frame
    // the device counts as sent arrives once and in order.
    {
        uint32_t lcg = 0x2468ACEu;
        auto next = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; };

        dev.on_byte = [](uint8_t b) { dev.reply((uint8_t)~b); };
        size_t first_sent = dev.sent_log.size();
        size_t first_recv = dev.received.size();
        std::vector<uint32_t> got;
        std::vector<uint8_t>  host_sent;

        for (int n = 0; n < 120; n++) {
            unsigned r = next() & 7;
            if (r < 4) {
                dev.send((uint8_t)next());
            } else if (r < 6) {
                if (!(rd(TX_DATA) & TX_BUSY)) {
                    uint8_t b = (uint8_t)next();
                    wr(TX_DATA, b);
                    host_sent.push_back(b);
                }
            }
            idle(next() % us(1500));
            uint32_t v = rd(RX_DATA);
            if (v & RX_VALID) got.push_back(v);
        }
        for (long t0 = now; now - t0 < us(200000); ) {
            uint32_t v = rd(RX_DATA);
            if (v & RX_VALID) { got.push_back(v); t0 = now; }
            if (dev.to_send.empty() && !(rd(TX_DATA) & TX_BUSY) && now - t0 > us(5000)) break;
            idle(200);
        }

        size_t sent = dev.sent_log.size() - first_sent;
        check(got.size() == sent, "at random: %zu bytes arrived, the device sent %zu",
              got.size(), sent);
        for (size_t i = 0; i < got.size() && i < sent; i++) {
            uint32_t want = RX_VALID | ((dev.sent_log[first_sent + i] >> 1) & 0xFF);
            check(got[i] == want, "at random, byte %zu: RX_DATA %03x, want %03x", i, got[i], want);
        }
        size_t recv = dev.received.size() - first_recv;
        check(recv == host_sent.size(), "at random: the device took %zu bytes of %zu",
              recv, host_sent.size());
        for (size_t i = 0; i < recv && i < host_sent.size(); i++)
            check(dev.received[first_recv + i] == host_sent[i],
                  "at random, byte %zu to the device: %02x, want %02x", i,
                  dev.received[first_recv + i], host_sent[i]);
        check(dev.bad_parity == 0 && dev.bad_stop == 0,
              "at random: %d bytes malformed at the device", dev.bad_parity + dev.bad_stop);
    }

    return finish();
}

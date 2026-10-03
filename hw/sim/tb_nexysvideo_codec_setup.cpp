// Verilator testbench for the Nexys Video's codec setup
//
// The testbench is the I2C master at the setup's command port, and
// behind it a model of the ADAU1761's control port: its registers,
// the six-byte PLL register, a PLL that locks a few milliseconds after
// it is enabled, and the codec's rule that only R0 and R1 may be
// touched while its core clock is off. The checks are what the board
// depends on: the registers the codec ends with; that it is never
// written while it cannot take it; that a running codec is muted
// before its clock stops; that its outputs stay muted through the
// precharge; that BUSY lasts no longer than the audio class allows;
// and that each way the bring-up can fail ends in FAULT with the code
// the setup documents.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <map>
#include <string>
#include <vector>
#include "Vnexysvideo_codec_setup_test.h"

namespace {

Vnexysvideo_codec_setup_test* dut;
int pass = 0, fail = 0;
long now = 0;                     // cycles; the harness clock is 1 MHz, so microseconds

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    va_list ap; va_start(ap, fmt);
    printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
    va_end(ap); fail++;
}

constexpr long MS = 1000;
constexpr long BUSY_LIMIT = 100 * MS;       // the audio class's bound on BUSY

constexpr uint16_t R0 = 0x4000, R1 = 0x4002, R29 = 0x4023, R35 = 0x4029;

// Fault codes, as the setup documents them: what failed, then the step.
constexpr int FAULT_NACK = 1, FAULT_BUS = 2, FAULT_LOCK = 3;
constexpr uint8_t fault_code(int what, int step) { return uint8_t((what << 6) | step); }

// ── The codec's control port ─────────────────────────────────
struct Codec {
    bool present = true;              // answers its address
    bool pll_can_lock = true;
    long lock_after = 3 * MS;         // the datasheet's typical lock time
    uint16_t nack_data_for = 0;       // refuses data for this register (0: none)

    std::map<uint16_t, uint8_t> reg;  // single-byte registers
    uint8_t r1[6] = {0x00, 0xFD, 0x00, 0x0C, 0x10, 0x00};
    bool    pll_on = false;
    long    pll_started = 0;
    long    t_powered = -100 * MS;    // playback power came on
    std::vector<std::string> broken;  // rules the setup broke

    enum St { NONE, ADDR, SUB_HI, SUB_LO, DATA, READ } st = NONE;
    uint16_t sub = 0;
    uint8_t  r1_in[6];
    int      r1_pos = 0;              // R1 bytes written so far
    int      r1_rd = 0;               // R1 bytes read so far

    bool locked() const { return pll_on && pll_can_lock && now - pll_started >= lock_after; }
    bool core_running() const { return (get(R0) & 0x01) && locked(); }
    uint8_t get(uint16_t a) const { auto it = reg.find(a); return it == reg.end() ? 0 : it->second; }
    bool live(int out) const { return (get(R29 + out) & 0x02) && (get(R35) & 0x03); }

    void rule(const char* fmt, ...) {
        char buf[160];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        broken.push_back(buf);
    }

    void write_reg(uint16_t a, uint8_t v) {
        if (a == R1) {
            r1_in[r1_pos++] = v;
            if (r1_pos < 6) return;
            r1_pos = 0; sub = R1 + 1;
            bool en = r1_in[5] & 1;
            if (en && !pll_on) pll_started = now;
            pll_on = en;
            for (int i = 0; i < 5; i++) r1[i] = r1_in[i];
            r1[5] = r1_in[5] & 1;
            return;
        }
        sub = a + 1;
        if (a != R0 && !core_running())
            rule("wrote %04X=%02X with the core clock not running", a, v);
        if (a == R0 && (get(R0) & 1) && !(v & 1))
            for (int o = 0; o < 4; o++)
                if (live(o)) rule("stopped the core clock with output %04X unmuted", R29 + o);
        bool was_live[4];
        for (int o = 0; o < 4; o++) was_live[o] = live(o);
        bool was_powered = get(R35) & 0x03;
        reg[a] = v;
        if (!was_powered && (get(R35) & 0x03)) t_powered = now;
        for (int o = 0; o < 4; o++)
            if (!was_live[o] && live(o) && now - t_powered < 4 * MS)
                rule("unmuted %04X %ld us after playback power, needs 4 ms",
                     R29 + o, now - t_powered);
    }

    void start() { st = ADDR; r1_pos = 0; r1_rd = 0; }
    void stop()  { st = NONE; }

    bool write_byte(uint8_t b) {      // true: acknowledged
        switch (st) {
            case ADDR:
                if (!present || (b >> 1) != 0x3B) { st = NONE; return false; }
                st = (b & 1) ? READ : SUB_HI;
                return true;
            case SUB_HI: sub = uint16_t(b << 8); st = SUB_LO; return true;
            case SUB_LO: sub |= b; st = DATA; return true;
            case DATA:
                if (sub == nack_data_for) return false;
                write_reg(sub, b);
                return true;
            default:
                rule("wrote %02X outside a register write", b);
                return false;
        }
    }

    uint8_t read_byte() {
        if (st != READ) { rule("read outside a register read"); return 0xFF; }
        if (sub == R1) {
            uint8_t v = r1[r1_rd];
            if (r1_rd == 5) v = uint8_t((v & 0x01) | (locked() ? 0x02 : 0));
            if (++r1_rd == 6) { r1_rd = 0; sub = R1 + 1; }
            return v;
        }
        return get(sub++);
    }
} codec;

// ── The I2C master's command port ────────────────────────────
// Takes a command when the setup offers one, works it against the codec
// and answers when the bus would have carried it at 100 kHz: nine bits
// of 10 us, a START's six 2.5 us steps and a STOP's five. A write the
// codec does not acknowledge ends in the master's own STOP.
constexpr int BYTE_US = 90, START_US = 15, STOP_US = 13;

struct Port {
    bool     busy = false;
    int      answer_in = 0;
    bool     nack = false, error = false;
    uint8_t  rdata = 0;
    uint16_t error_for = 0;           // a bus error on this register's data byte
    int      commands = 0;
} port;

void take(bool start, bool stop, bool read, uint8_t wdata, bool nack) {
    port.commands++;
    port.nack = false; port.error = false; port.rdata = 0;
    if (start) codec.start();
    if (port.error_for && codec.st == Codec::DATA && codec.sub == port.error_for) {
        port.error = true;
        codec.stop();
    } else if (read) {
        port.rdata = codec.read_byte();
        (void)nack;
    } else {
        port.nack = !codec.write_byte(wdata);
        if (port.nack) codec.stop();
    }
    if (stop && !port.nack && !port.error) codec.stop();
    port.busy = true;
    port.answer_in = BYTE_US + (start ? START_US : 0) + ((stop || port.nack) ? STOP_US : 0);
}

void tick() {
    bool offer = dut->o_cmd_valid && !port.busy;
    bool c_start = dut->o_cmd_start, c_stop = dut->o_cmd_stop, c_read = dut->o_cmd_read;
    bool c_nack = dut->o_cmd_nack;
    uint8_t c_wdata = dut->o_cmd_wdata;

    dut->i_cmd_ready = !port.busy;
    bool answering = port.busy && port.answer_in == 0;
    dut->i_rsp_valid = answering;
    dut->i_rsp_nack  = answering && port.nack;
    dut->i_rsp_error = answering && port.error;
    dut->i_rsp_rdata = answering ? port.rdata : 0;

    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();

    if (answering) port.busy = false;
    else if (port.busy) port.answer_in--;
    if (offer) take(c_start, c_stop, c_read, c_wdata, c_nack);
    now++;
}

void reset() {
    dut->i_rst = 1;
    for (int i = 0; i < 4; i++) tick();
    dut->i_rst = 0;
}

// Runs the setup from reset to done or fault; returns how long BUSY was up.
long run() {
    reset();
    long t0 = now;
    check(dut->o_busy && !dut->o_fault, "not BUSY coming out of reset");
    while (dut->o_busy && now - t0 < 2 * BUSY_LIMIT) tick();
    return now - t0;
}

// The registers the codec ends with: every one the setup writes.
const std::map<uint16_t, uint8_t> FINAL = {
    {0x4000, 0x0F},                                   // PLL clock, core on
    {0x4015, 0x01}, {0x4016, 0x00}, {0x4017, 0x00},   // I2S, codec drives BCLK/LRCLK, 48 kHz
    {0x40F8, 0x00}, {0x40F4, 0x00}, {0x40F2, 0x01},   // serial pins, input to the DACs
    {0x40F5, 0x00}, {0x40F6, 0x00}, {0x40F7, 0x00},   // DSP off, codec slew
    {0x4036, 0x00}, {0x40F9, 0x7F}, {0x40FA, 0x03},   // dejitter bypassed, clocks on
    {0x402B, 0x00}, {0x402C, 0x00}, {0x402A, 0x03},   // DACs at 0 dB, both on
    {0x401C, 0x21}, {0x401D, 0x00}, {0x401E, 0x41}, {0x401F, 0x00},
    {0x4020, 0x05}, {0x4021, 0x11}, {0x4022, 0x00},   // line mixers at 0 dB, mono off
    {0x4027, 0x00}, {0x4028, 0x00}, {0x4029, 0x03},
    {0x4023, 0xBF}, {0x4024, 0xBF},                   // headphones -10 dB, unmuted
    {0x4025, 0xE6}, {0x4026, 0xE6},                   // line out 0 dB, unmuted
};

void check_final(const char* how) {
    for (auto [a, v] : FINAL)
        check(codec.get(a) == v, "%s: %04X is %02X, expected %02X", how, a, codec.get(a), v);
    // 12.5 MHz x (3 + 2913/3125) = 49.152 MHz, fractional mode, enabled.
    const uint8_t pll[6] = {0x0C, 0x35, 0x0B, 0x61, 0x19, 0x01};
    bool pll_ok = true;
    for (int i = 0; i < 6; i++) pll_ok = pll_ok && codec.r1[i] == pll[i];
    check(pll_ok, "%s: R1 is %02X %02X %02X %02X %02X %02X", how,
          codec.r1[0], codec.r1[1], codec.r1[2], codec.r1[3], codec.r1[4], codec.r1[5]);
    check(codec.locked(), "%s: PLL not locked at the end", how);
}

void check_rules(const char* how) {
    for (const std::string& b : codec.broken)
        check(false, "%s: %s", how, b.c_str());
    codec.broken.clear();
}

void fresh_codec() { codec = Codec(); }

} // namespace

int main() {
    dut = new Vnexysvideo_codec_setup_test;

    // ── A codec from power-on ────────────────────────────────
    fresh_codec();
    long busy = run();
    check(!dut->o_busy && !dut->o_fault, "cold: ended busy %d fault %d code %02X",
          dut->o_busy, dut->o_fault, dut->o_fault_code);
    check(busy <= BUSY_LIMIT, "cold: BUSY for %ld ms, the class allows 100", busy / MS);
    check_final("cold");
    check_rules("cold");
    printf("  cold bring-up: %ld ms, %d commands\n", busy / MS, port.commands);

    // ── A codec still running from an earlier bitstream ──────
    // Its PLL locked long ago, its core clock on, its outputs powered
    // and unmuted: it is muted before its clock is stopped.
    fresh_codec();
    codec.reg[R0] = 0x0F;
    codec.pll_on = true; codec.pll_started = now - 100 * MS;
    codec.reg[R35] = 0x03;
    for (int o = 0; o < 4; o++) codec.reg[R29 + o] = 0xFF;
    busy = run();
    check(!dut->o_busy && !dut->o_fault, "warm: ended busy %d fault %d code %02X",
          dut->o_busy, dut->o_fault, dut->o_fault_code);
    check(busy <= BUSY_LIMIT, "warm: BUSY for %ld ms, the class allows 100", busy / MS);
    check_final("warm");
    check_rules("warm");

    // ── No codec answering ───────────────────────────────────
    // The first byte on the bus goes unacknowledged.
    fresh_codec();
    codec.present = false;
    run();
    check(dut->o_fault && !dut->o_busy, "absent codec: no FAULT");
    check(dut->o_fault_code == fault_code(FAULT_NACK, 0),
          "absent codec: code %02X, expected %02X", dut->o_fault_code, fault_code(FAULT_NACK, 0));

    // ── A PLL that never locks ───────────────────────────────
    fresh_codec();
    codec.pll_can_lock = false;
    busy = run();
    check(dut->o_fault && dut->o_fault_code == fault_code(FAULT_LOCK, 8),
          "no lock: fault %d code %02X, expected %02X", dut->o_fault, dut->o_fault_code,
          fault_code(FAULT_LOCK, 8));
    check(busy <= BUSY_LIMIT, "no lock: BUSY for %ld ms, the class allows 100", busy / MS);
    check_rules("no lock");

    // ── A byte refused, and a bus that stops ─────────────────
    // Each names the step it happened in: mixer 5 is step 33, mixer 3
    // step 29.
    fresh_codec();
    codec.nack_data_for = 0x4020;
    run();
    check(dut->o_fault && dut->o_fault_code == fault_code(FAULT_NACK, 33),
          "refused byte: code %02X, expected %02X", dut->o_fault_code, fault_code(FAULT_NACK, 33));

    fresh_codec();
    port.error_for = 0x401C;
    run();
    port.error_for = 0;
    check(dut->o_fault && dut->o_fault_code == fault_code(FAULT_BUS, 29),
          "bus error: code %02X, expected %02X", dut->o_fault_code, fault_code(FAULT_BUS, 29));

    // A fault holds until reset, and the next reset tries again.
    for (int i = 0; i < 1000; i++) tick();
    check(dut->o_fault && !dut->o_busy, "a fault did not hold");
    fresh_codec();
    run();
    check(!dut->o_busy && !dut->o_fault, "no bring-up after the reset that followed a fault");
    check_rules("after a fault");

    printf("\nnexysvideo_codec_setup: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

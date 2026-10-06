// Verilator testbench for the Ethernet transmitter
//
// The transmitter reads a modelled slot memory and sends across the seam
// to a monitor that takes a byte on every enable, as a PHY adapter does.
// The checks are the ones the transmit ring's contract makes: every queued
// frame goes out whole and in order behind a preamble, padded to the
// shortest legal frame and closed by a correct check sequence; the wire
// idles twelve byte times between frames; TX_CONS counts a frame only once
// its last byte has gone, never moves by more than one a cycle and never
// passes TX_PROD; a slot with a bad LEN sends nothing; and with TX_EN
// clear the frame on the wire finishes and the rest of the queue is
// discarded. All of it holds whatever the spacing of the enables.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include <vector>
#include "Veth_tx.h"

namespace {

Veth_tx* dut;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    // A transmitter this wrong has nothing more to say, and the waits
    // ahead would each run to their limit.
    if (fail >= 500) {
        printf("eth_tx: gave up after %d failures\n", fail);
        exit(1);
    }
}

// The module's defaults.
constexpr int SLOTS      = 8;
constexpr int SLOT_BYTES = 2048;
constexpr int MAX_LEN    = SLOT_BYTES - 2;
constexpr int MIN_FRAME  = 60;
constexpr int GAP_BYTES  = 12;

// ── The slot memory ──────────────────────────────────────────
// Words, little-endian, read a cycle after the address.
uint32_t mem[SLOTS * SLOT_BYTES / 4];

void slot_write(int slot, int offset, uint8_t byte) {
    uint32_t& w = mem[(slot * SLOT_BYTES + offset) / 4];
    int shift = 8 * (offset % 4);
    w = (w & ~(0xffu << shift)) | ((uint32_t)byte << shift);
}

// ── Reference ────────────────────────────────────────────────
uint32_t crc32_remainder(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t b : data)
        for (int i = 0; i < 8; i++) {
            uint32_t feedback = (crc ^ (b >> i)) & 1u;
            crc = (crc >> 1) ^ (feedback ? 0xedb88320u : 0u);
        }
    return crc;
}

// What a frame of these bytes is on the wire.
std::vector<uint8_t> wire_frame(std::vector<uint8_t> frame) {
    while (frame.size() < MIN_FRAME) frame.push_back(0);
    uint32_t fcs = ~crc32_remainder(frame);
    std::vector<uint8_t> wire(7, 0x55);
    wire.push_back(0xd5);
    wire.insert(wire.end(), frame.begin(), frame.end());
    for (int i = 0; i < 4; i++) wire.push_back((uint8_t)(fcs >> (8 * i)));
    return wire;
}

// ── What software queued ─────────────────────────────────────
struct Queued {
    uint16_t             count;     // the TX_PROD count the slot was queued at
    std::vector<uint8_t> wire;      // empty: the slot sends nothing
};
std::deque<Queued> expected;        // frames still to appear on the wire
uint16_t prod = 0;

// ── The seam's monitor ───────────────────────────────────────
struct Monitor {
    std::vector<uint8_t> frame;     // the frame being taken
    bool in_frame = false;
    long idle = 1000;               // byte times idle since the last frame
    long last_gap = -1;             // the gap ahead of the last frame started
    int  frames = 0;
    uint16_t prev_cons = 0;

    // A byte time: the adapter takes what the seam holds. cons_before is
    // TX_CONS as it stood when the byte was taken, cons what the same
    // edge left it at.
    void take(bool tx_en, uint8_t txd, uint16_t cons_before, uint16_t cons) {
        if (tx_en) {
            if (!in_frame) {
                in_frame = true;
                frame.clear();
                last_gap = idle;
                check(idle >= GAP_BYTES, "frame %d after a gap of %ld byte times",
                      frames, idle);
                check(!expected.empty(), "frame %d with nothing queued", frames);
            }
            // Not counted while any byte of it is still to go.
            if (!expected.empty())
                check(cons_before == expected.front().count,
                      "TX_CONS %u with byte %zu of frame %d (count %u) to go",
                      cons_before, frame.size(), frames, expected.front().count);
            frame.push_back(txd);
            return;
        }
        idle = in_frame ? 1 : idle + 1;
        if (!in_frame) return;
        in_frame = false;
        frames++;
        if (expected.empty()) return;
        const Queued& q = expected.front();
        check(frame.size() == q.wire.size(),
              "frame %d (count %u): %zu bytes on the wire, want %zu", frames - 1,
              q.count, frame.size(), q.wire.size());
        // The first byte that differs, counted from the preamble's first:
        // the slot's bytes start at 8.
        size_t at = 0;
        while (at < frame.size() && at < q.wire.size() && frame[at] == q.wire[at]) at++;
        if (at < frame.size() && at < q.wire.size())
            check(false, "frame %d (count %u): byte %zu on the wire is %02x, want %02x",
                  frames - 1, q.count, at, frame[at], q.wire[at]);
        else
            check(true, "");
        check((uint16_t)(cons - q.count) >= 1, "TX_CONS %u after frame of count %u",
              cons, q.count);
        expected.pop_front();
    }
} mon;

// ── The clock ────────────────────────────────────────────────
int  ce_period = 1;                 // an enable every this many cycles
bool ce_random = false;
uint32_t lcg = 0x1234567u;
uint32_t next_random() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

void tick() {
    bool ce = ce_random ? (next_random() % 3 == 0) : (now % ce_period == 0);
    dut->i_ce = ce;
    dut->i_clk = 0;
    dut->eval();

    // What the edge will act on.
    uint32_t addr        = dut->o_mem_addr;
    bool     tx_en       = dut->o_tx_en;
    uint8_t  txd         = dut->o_txd;
    uint16_t cons_before = dut->o_cons;

    dut->i_clk = 1;
    dut->eval();
    now++;

    dut->i_mem_rdata = mem[addr];

    // A reset is the one thing that moves the count other than by one,
    // and the one thing that cuts a frame.
    uint16_t cons = dut->o_cons;
    if (dut->i_rst) {
        mon.prev_cons = cons;
        return;
    }
    check((uint16_t)(cons - mon.prev_cons) <= 1, "TX_CONS moved %u to %u in a cycle",
          mon.prev_cons, cons);
    check((uint16_t)(prod - cons) <= SLOTS, "TX_CONS %u passed TX_PROD %u", cons, prod);
    mon.prev_cons = cons;

    if (ce) mon.take(tx_en, txd, cons_before, cons);
}

void run(int cycles) { for (int i = 0; i < cycles; i++) tick(); }

// Queue one slot: its LEN and, where the LEN is one a slot can hold, a
// frame of that many bytes. Waits for room in the ring first.
void queue(int len, uint8_t seed, bool sends = true) {
    long t0 = now;
    while ((uint16_t)(prod - (uint16_t)dut->o_cons) >= SLOTS && now - t0 < 2000000)
        tick();
    check((uint16_t)(prod - (uint16_t)dut->o_cons) < SLOTS, "ring never drained");

    int slot = prod % SLOTS;
    std::vector<uint8_t> frame;
    if (len >= 1 && len <= MAX_LEN) {
        for (int i = 0; i < len; i++) {
            frame.push_back((uint8_t)(seed + 7 * i + (i >> 8)));
            slot_write(slot, 2 + i, frame.back());
        }
    }
    slot_write(slot, 0, (uint8_t)len);
    slot_write(slot, 1, (uint8_t)(len >> 8));

    if (sends && len >= 1 && len <= MAX_LEN)
        expected.push_back({prod, wire_frame(frame)});
    prod++;
    dut->i_prod = prod;
}

// Run until the ring is empty and the wire has been idle a while.
void drain() {
    long t0 = now;
    while (((uint16_t)dut->o_cons != prod || mon.in_frame) && now - t0 < 4000000)
        tick();
    check((uint16_t)dut->o_cons == prod, "ring not drained: TX_CONS %u, TX_PROD %u",
          (uint16_t)dut->o_cons, prod);
    run(40 * (ce_random ? 3 : ce_period));
    check(expected.empty(), "%zu queued frames never appeared", expected.size());
    expected.clear();
}

// Run until the frame on the wire has had this many bytes taken, the
// preamble's among them.
void run_into_frame(size_t bytes) {
    long t0 = now;
    while ((!mon.in_frame || mon.frame.size() < bytes) && now - t0 < 200000) tick();
    check(mon.in_frame && mon.frame.size() >= bytes,
          "no frame reached %zu bytes on the wire", bytes);
}

void reset() {
    dut->i_rst = 1;
    prod = 0;
    dut->i_prod = 0;
    expected.clear();
    tick();
    tick();
    dut->i_rst = 0;
    mon.in_frame = false;
    mon.idle = 1000;
    mon.prev_cons = 0;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_tx;
    dut->i_clk = 0;
    dut->i_enable = 0;
    dut->i_ce = 0;
    dut->i_mem_rdata = 0;
    reset();

    const int lengths[] = {1, 2, 3, 4, 5, 6, 7, 58, 59, 60, 61, 62, 63, 64, 65,
                           1514, MAX_LEN};

    // Every length that matters, at every spacing of the enables: a byte
    // a cycle, as at 1000 Mbit/s; every other cycle; every eighth; and at
    // random.
    for (int mode = 0; mode < 4; mode++) {
        ce_random = (mode == 3);
        ce_period = (mode == 0) ? 1 : (mode == 1) ? 2 : 8;
        dut->i_enable = 1;
        int frames_before = mon.frames;
        uint8_t seed = (uint8_t)(0x11 * (mode + 1));
        for (int len : lengths)
            queue(len, seed++);
        drain();
        check(mon.frames - frames_before == (int)(sizeof lengths / sizeof *lengths),
              "mode %d: %d frames on the wire", mode, mon.frames - frames_before);
    }

    // Frames queued together leave with exactly the gap between them.
    for (int period : {1, 2, 8}) {
        ce_random = false;
        ce_period = period;
        run(100 * period);
        for (int i = 0; i < 3; i++) queue(64 + i, (uint8_t)(0x40 + i));
        long t0 = now;
        int  frames_before = mon.frames;
        while (mon.frames - frames_before < 2 && now - t0 < 100000) {
            tick();
            if (mon.in_frame && mon.frame.size() == 1 && mon.frames > frames_before)
                check(mon.last_gap == GAP_BYTES,
                      "gap of %ld byte times between frames queued together, "
                      "an enable every %d", mon.last_gap, period);
        }
        drain();
    }

    // A LEN no slot can hold, and none at all, send nothing and are passed
    // over; the frames around them are not disturbed.
    {
        ce_period = 2;
        int frames_before = mon.frames;
        queue(100, 0x51);
        queue(0, 0);
        queue(MAX_LEN + 1, 0);
        queue(101, 0x52);
        queue(SLOT_BYTES, 0);
        queue(0xffff, 0);
        queue(102, 0x53);
        drain();
        check(mon.frames - frames_before == 3, "%d frames among bad slots",
              mon.frames - frames_before);
    }

    // TX_EN cleared with a frame on the wire: that frame ends whole, the
    // rest of the queue goes unsent, and the ring is empty.
    {
        ce_period = 1;
        int frames_before = mon.frames;
        queue(300, 0x61);
        run_into_frame(50);
        // Queued behind it, never to be sent.
        for (int i = 0; i < 4; i++) queue(200 + i, (uint8_t)(0x70 + i), false);
        dut->i_enable = 0;
        drain();
        check(mon.frames - frames_before == 1, "%d frames after TX_EN cleared",
              mon.frames - frames_before);

        // Frames queued while it is clear go the same way.
        for (int i = 0; i < 3; i++) queue(90 + i, (uint8_t)(0x80 + i), false);
        drain();
        check(mon.frames - frames_before == 1, "%d frames sent with TX_EN clear",
              mon.frames - frames_before);

        // And the next start sends from where the counts stand.
        dut->i_enable = 1;
        queue(77, 0x91);
        queue(78, 0x92);
        drain();
        check(mon.frames - frames_before == 3, "%d frames after the restart",
              mon.frames - frames_before);
    }

    // The counts wrap at 65536 with nothing to do about it: pass that
    // many slots over, then send across the wrap.
    {
        ce_period = 1;
        dut->i_enable = 1;
        int frames_before = mon.frames;
        while (prod < 65530 && prod >= 8) queue(0, 0);
        for (int i = 0; i < 12; i++) queue(60 + i, (uint8_t)(0xa0 + i));
        drain();
        check(prod < 100, "TX_PROD did not wrap: %u", prod);
        check(mon.frames - frames_before == 12, "%d frames across the counts' wrap",
              mon.frames - frames_before);
    }

    // A reset mid-frame stops the wire and clears the count.
    {
        ce_period = 2;
        queue(500, 0xb1);
        run_into_frame(100);
        reset();
        dut->eval();
        check(!dut->o_tx_en && (uint16_t)dut->o_cons == 0,
              "after reset: tx_en %d, TX_CONS %u", dut->o_tx_en, (uint16_t)dut->o_cons);
        run(200);
        int frames_before = mon.frames;
        queue(70, 0xc1);
        drain();
        check(mon.frames - frames_before == 1, "%d frames after reset",
              mon.frames - frames_before);
    }

    printf("eth_tx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

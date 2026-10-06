// Verilator testbench for the Ethernet receiver
//
// The receiver is fed byte times across the seam, as a PHY adapter hands
// them over, and writes a modelled slot memory. Each frame sent is held to
// what the receive ring's contract says becomes of it: stored whole with
// its length, in the slot RX_PROD names, before RX_PROD counts it; lost to
// a full ring and counted; or discarded without a trace. The cases are the
// contract's three tests one at a time and then together at random: well
// formed (check sequence, length, the PHY's error), addressed to this
// station under every filter setting, and a slot free. All of it holds
// whatever the spacing of the enables.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include <vector>
#include "Veth_rx.h"

namespace {

Veth_rx* dut;
int pass = 0, fail = 0;
long now = 0;                       // clock cycles since the start

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    // A receiver this wrong has nothing more to say.
    if (fail >= 500) {
        printf("eth_rx: gave up after %d failures\n", fail);
        exit(1);
    }
}

// The module's defaults.
constexpr int SLOTS      = 32;
constexpr int SLOT_BYTES = 2048;
constexpr int MIN_FRAME  = 60;
constexpr int MAX_FRAME  = 1518;

typedef std::vector<uint8_t> Bytes;

const uint8_t STATION[6]   = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t GROUP[6]     = {0x01, 0x00, 0x5e, 0x12, 0x34, 0x56};
const uint8_t OTHER[6]     = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};

// ── The slot memory ──────────────────────────────────────────
uint32_t mem[SLOTS * SLOT_BYTES / 4];
long     writes = 0;

uint8_t slot_byte(int slot, int offset) {
    return (uint8_t)(mem[(slot * SLOT_BYTES + offset) / 4] >> (8 * (offset % 4)));
}

// ── Reference ────────────────────────────────────────────────
uint32_t crc32_remainder(const Bytes& data) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t b : data)
        for (int i = 0; i < 8; i++) {
            uint32_t feedback = (crc ^ (b >> i)) & 1u;
            crc = (crc >> 1) ^ (feedback ? 0xedb88320u : 0u);
        }
    return crc;
}

uint32_t lcg = 0x1234567u;
uint32_t next_random() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

// A frame of `len` bytes to `dest`, without its check sequence.
Bytes make_frame(const uint8_t* dest, int len) {
    Bytes f(dest, dest + 6);
    while ((int)f.size() < len) f.push_back((uint8_t)next_random());
    f.resize(len);
    return f;
}

// ── The wire ─────────────────────────────────────────────────
struct ByteTime { uint8_t rxd; bool dv, er; };
std::deque<ByteTime> wire;

struct Shape {
    int  preamble  = 7;     // bytes of 0x55 ahead of the delimiter
    int  gap       = 12;    // idle byte times behind the frame
    bool bad_fcs   = false;
    int  er_at     = -1;    // the PHY flags this byte of the frame
    int  cut_at    = -1;    // the frame ends here, short of its check sequence
    bool junk_head = false; // something other than preamble ahead of the delimiter
};

void put_on_wire(const Bytes& frame, const Shape& s = Shape()) {
    if (s.junk_head) wire.push_back({0x12, true, false});
    for (int i = 0; i < s.preamble; i++) wire.push_back({0x55, true, false});
    wire.push_back({0xd5, true, false});
    Bytes body = frame;
    uint32_t fcs = ~crc32_remainder(frame);
    if (s.bad_fcs) fcs ^= 0x00010000u;
    for (int i = 0; i < 4; i++) body.push_back((uint8_t)(fcs >> (8 * i)));
    if (s.cut_at >= 0) body.resize(s.cut_at);
    for (int i = 0; i < (int)body.size(); i++)
        wire.push_back({body[i], true, i == s.er_at});
    for (int i = 0; i < s.gap; i++) wire.push_back({0xdd, false, false});
}

// ── What software sees ───────────────────────────────────────
uint16_t prod_seen = 0;             // RX_PROD as last read
uint16_t cons = 0;                  // RX_CONS as last written
long     drops = 0;
std::deque<Bytes> awaited;          // the frames the next counts should be for, in order

// Release up to n of the frames held. Never more than are held: software
// writes RX_CONS no further than RX_PROD, and a case that stored nothing
// where it should have must not push it past.
void release(int n) {
    int held = (uint16_t)(prod_seen - cons);
    cons += (n < held) ? n : held;
    dut->i_cons = cons;
}

// ── The clock ────────────────────────────────────────────────
int  ce_period = 1;                 // an enable every this many cycles
bool ce_random = false;

void tick() {
    bool ce = ce_random ? (next_random() % 3 == 0) : (now % ce_period == 0);
    dut->i_ce = ce;
    if (ce) {
        ByteTime b = {0xdd, false, false};
        if (!wire.empty()) { b = wire.front(); wire.pop_front(); }
        dut->i_rxd = b.rxd;
        dut->i_dv  = b.dv;
        dut->i_er  = b.er;
    }
    dut->i_clk = 0;
    dut->eval();

    // The write the edge will make.
    bool     we    = dut->o_mem_we;
    uint32_t addr  = dut->o_mem_addr;
    uint32_t wdata = dut->o_mem_wdata;

    dut->i_clk = 1;
    dut->eval();
    now++;

    if (dut->i_rst) return;

    if (we) {
        check((int)(addr / (SLOT_BYTES / 4)) == prod_seen % SLOTS,
              "write to slot %u with RX_PROD %u", addr / (SLOT_BYTES / 4), prod_seen);
        check((uint16_t)(prod_seen - cons) < SLOTS, "write into a full ring");
        mem[addr] = wdata;
        writes++;
    }
    if (dut->o_drop) drops++;

    uint16_t prod = dut->o_prod;
    if (prod == prod_seen) return;
    check((uint16_t)(prod - prod_seen) == 1, "RX_PROD moved %u to %u in a cycle",
          prod_seen, prod);
    check((uint16_t)(prod - cons) <= SLOTS, "RX_PROD %u passed RX_CONS %u + SLOTS",
          prod, cons);

    // The slot is whole the moment it is counted.
    int slot = prod_seen % SLOTS;
    prod_seen = prod;
    check(!awaited.empty(), "a frame counted that should not have been stored");
    if (awaited.empty()) return;
    const Bytes& want = awaited.front();
    int len = slot_byte(slot, 0) | (slot_byte(slot, 1) << 8);
    check(len == (int)want.size(), "LEN %d for a frame of %zu bytes", len, want.size());
    size_t at = 0;
    while (at < want.size() && slot_byte(slot, 2 + (int)at) == want[at]) at++;
    check(at == want.size(), "slot %d: byte %zu of the frame is %02x, want %02x", slot,
          at, at < want.size() ? slot_byte(slot, 2 + (int)at) : 0,
          at < want.size() ? want[at] : 0);
    awaited.pop_front();
}

void run(int cycles) { for (int i = 0; i < cycles; i++) tick(); }

enum Outcome { STORED, DROPPED, DISCARDED };
const char* name(Outcome o) {
    return o == STORED ? "stored" : o == DROPPED ? "dropped" : "discarded";
}

// Put one frame through and hold the receiver to what should become of
// it. The ring's state is taken as the frame starts.
void deliver(const Bytes& frame, Outcome want, const char* what,
             const Shape& shape = Shape()) {
    if (want == STORED && (uint16_t)(prod_seen - cons) == SLOTS) want = DROPPED;

    uint16_t prod_before = prod_seen;
    long drops_before = drops, writes_before = writes;
    awaited.clear();
    if (want == STORED) awaited.push_back(frame);

    put_on_wire(frame, shape);
    long t0 = now;
    while (!wire.empty() && now - t0 < 400000) tick();
    run(8);

    Outcome got = (prod_seen != prod_before) ? STORED
                : (drops != drops_before)    ? DROPPED : DISCARDED;
    check(got == want, "%s, %zu bytes: %s, want %s", what, frame.size(), name(got),
          name(want));
    check((uint16_t)(prod_seen - prod_before) <= 1 && drops - drops_before <= 1,
          "%s: counted %u frames and %ld drops", what,
          (uint16_t)(prod_seen - prod_before), drops - drops_before);
    if (want == DROPPED)
        check(writes == writes_before, "%s: %ld words written with no slot free", what,
              writes - writes_before);
    awaited.clear();
}

void reset() {
    dut->i_rst = 1;
    wire.clear();
    tick();
    tick();
    dut->i_rst = 0;
    prod_seen = 0;
    cons = 0;
    dut->i_cons = 0;
    awaited.clear();
}

void set_mode(int mode) {
    ce_random = (mode == 3);
    ce_period = (mode == 0) ? 1 : (mode == 1) ? 2 : 8;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_rx;
    dut->i_clk = 0;
    dut->i_ce = 0;
    dut->i_rxd = 0;
    dut->i_dv = 0;
    dut->i_er = 0;
    dut->i_enable = 1;
    dut->i_promisc = 0;
    dut->i_allmulti = 0;
    dut->i_addr = 0x554433221102ULL;
    reset();

    // Every length at the edges, and every alignment of the last word, at
    // every spacing of the enables: a byte a cycle, as at 1000 Mbit/s;
    // every other cycle; every eighth; and at random.
    for (int mode = 0; mode < 4; mode++) {
        set_mode(mode);
        for (int len : {60, 61, 62, 63, 64, 65, 66, 67, 100, 1514, MAX_FRAME}) {
            deliver(make_frame(STATION, len), STORED, "to the station");
            release(1);
        }
        deliver(make_frame(STATION, MIN_FRAME - 1), DISCARDED, "one byte short");
        deliver(make_frame(STATION, MAX_FRAME + 1), DISCARDED, "one byte long");
        deliver(make_frame(STATION, 2000), DISCARDED, "far too long");
        deliver(make_frame(STATION, 80), STORED, "after frames not to be had");
        release(1);
    }

    // Addressed to this station: under each filter setting, the station's
    // own address and the broadcast always, a group address with ALLMULTI,
    // anything with PROMISC. An address one byte off the station's is
    // another station's, whichever byte it is.
    set_mode(1);
    for (int setting = 0; setting < 4; setting++) {
        bool promisc = setting & 1, allmulti = setting & 2;
        dut->i_promisc = promisc;
        dut->i_allmulti = allmulti;
        deliver(make_frame(STATION, 64), STORED, "to the station");
        release(1);
        deliver(make_frame(BROADCAST, 64), STORED, "broadcast");
        release(1);
        Outcome group = (promisc || allmulti) ? STORED : DISCARDED;
        deliver(make_frame(GROUP, 64), group, "to a group");
        if (group == STORED) release(1);
        Outcome other = promisc ? STORED : DISCARDED;
        deliver(make_frame(OTHER, 64), other, "to another station");
        if (other == STORED) release(1);
        for (int i = 0; i < 6; i++) {
            uint8_t near[6];
            for (int j = 0; j < 6; j++) near[j] = STATION[j];
            near[i] ^= 0x40;
            deliver(make_frame(near, 64), other, "one byte off the station's");
            if (other == STORED) release(1);
            uint8_t almost[6];
            for (int j = 0; j < 6; j++) almost[j] = 0xff;
            almost[i] = 0xfe;
            // All ones but one bit: a group address unless the bit is the
            // group bit itself.
            Outcome want = promisc ? STORED : (i == 0) ? DISCARDED : group;
            deliver(make_frame(almost, 64), want, "one bit off the broadcast");
            if (want == STORED) release(1);
        }
    }
    dut->i_promisc = 0;
    dut->i_allmulti = 0;

    // Well formed: a wrong check sequence, a byte the PHY flagged, a frame
    // cut short and one with no proper preamble all vanish, and the frame
    // behind each is taken as usual.
    for (int mode = 0; mode < 4; mode++) {
        set_mode(mode);
        Shape s;
        s.bad_fcs = true;
        deliver(make_frame(STATION, 200), DISCARDED, "wrong check sequence", s);
        s = Shape();
        s.er_at = 100;
        deliver(make_frame(STATION, 200), DISCARDED, "flagged by the PHY", s);
        s = Shape();
        s.cut_at = 150;
        deliver(make_frame(STATION, 200), DISCARDED, "cut short", s);
        s = Shape();
        s.junk_head = true;
        deliver(make_frame(STATION, 200), DISCARDED, "no proper preamble", s);
        deliver(make_frame(STATION, 200), STORED, "after malformed frames");
        release(1);

        // However much of the preamble the PHY passes on.
        for (int preamble = 0; preamble <= 7; preamble++) {
            s = Shape();
            s.preamble = preamble;
            deliver(make_frame(STATION, 64 + preamble), STORED, "short preamble", s);
            release(1);
        }
    }

    // Frames one behind another, with two byte times of gap and one of
    // preamble between them and a byte every cycle: each is committed in
    // the gap, and the next is not missed for it.
    {
        set_mode(0);
        Shape s;
        s.gap = 2;
        s.preamble = 1;
        uint16_t prod_before = prod_seen;
        for (int i = 0; i < 20; i++) {
            Bytes f = make_frame(STATION, 60 + i);
            awaited.push_back(f);
            put_on_wire(f, s);
        }
        long t0 = now;
        while (!wire.empty() && now - t0 < 400000) tick();
        run(8);
        check((uint16_t)(prod_seen - prod_before) == 20 && awaited.empty(),
              "%u of 20 frames close behind one another stored",
              (uint16_t)(prod_seen - prod_before));
        awaited.clear();
        release(20);
    }

    // A slot free: fill the ring, and a frame that would be received is
    // counted as dropped with nothing written. One that would not be is
    // not counted. A slot released takes the next.
    for (int mode : {0, 2}) {
        set_mode(mode);
        for (int i = 0; i < SLOTS; i++)
            deliver(make_frame(STATION, 60 + i), STORED, "filling the ring");
        check((uint16_t)(prod_seen - cons) == SLOTS, "ring not full: %u frames",
              (uint16_t)(prod_seen - cons));
        deliver(make_frame(STATION, 300), DROPPED, "to a full ring");
        deliver(make_frame(BROADCAST, 300), DROPPED, "broadcast to a full ring");
        deliver(make_frame(OTHER, 300), DISCARDED, "to another station, ring full");
        Shape s;
        s.bad_fcs = true;
        deliver(make_frame(STATION, 300), DISCARDED, "malformed, ring full", s);
        release(1);
        deliver(make_frame(STATION, 301), STORED, "into the slot released");
        deliver(make_frame(STATION, 302), DROPPED, "ring full again");
        release(SLOTS);
    }

    // RX_EN clear: nothing is received and nothing counted.
    {
        set_mode(1);
        dut->i_enable = 0;
        deliver(make_frame(STATION, 100), DISCARDED, "with RX_EN clear");
        deliver(make_frame(BROADCAST, 100), DISCARDED, "broadcast with RX_EN clear");
        dut->i_enable = 1;
        deliver(make_frame(STATION, 100), STORED, "with RX_EN set again");
        release(1);
    }

    // A reset mid-frame leaves nothing of it, and the counts at zero.
    {
        set_mode(1);
        Bytes f = make_frame(STATION, 400);
        put_on_wire(f);
        run(300);
        reset();
        dut->eval();
        check((uint16_t)dut->o_prod == 0 && !dut->o_mem_we, "after reset: RX_PROD %u",
              (uint16_t)dut->o_prod);
        run(50);
        deliver(make_frame(STATION, 90), STORED, "after reset");
        release(1);
    }

    // Everything at once, at random: lengths, destinations, filter
    // settings, malformed frames, and a consumer that sometimes falls
    // behind and lets the ring fill.
    for (int n = 0; n < 1500; n++) {
        if (n % 100 == 0) set_mode(next_random() % 4);
        bool promisc = (next_random() % 8 == 0), allmulti = (next_random() % 4 == 0);
        dut->i_promisc = promisc;
        dut->i_allmulti = allmulti;

        int kind = next_random() % 4;
        const uint8_t* dest = kind == 0 ? STATION : kind == 1 ? BROADCAST
                            : kind == 2 ? GROUP : OTHER;
        bool addressed = kind < 2 || promisc || (kind == 2 && allmulti);

        int len = (next_random() % 8 == 0) ? (int)(next_random() % 1600) + 6
                                           : MIN_FRAME + (int)(next_random() % 200);
        Shape s;
        s.preamble = 1 + next_random() % 7;
        s.gap = 2 + next_random() % 14;
        s.bad_fcs = (next_random() % 10 == 0);
        if (next_random() % 12 == 0) s.er_at = next_random() % len;
        bool well_formed = len >= MIN_FRAME && len <= MAX_FRAME && !s.bad_fcs &&
                           s.er_at < 0;

        char what[80];
        snprintf(what, sizeof what, "random, %s%s%s%s",
                 kind == 0 ? "to the station" : kind == 1 ? "broadcast"
                 : kind == 2 ? "to a group" : "to another station",
                 promisc ? ", PROMISC" : "", allmulti ? ", ALLMULTI" : "",
                 well_formed ? "" : ", malformed");
        deliver(make_frame(dest, len), (well_formed && addressed) ? STORED : DISCARDED,
                what, s);
        // Release some of what is held, more often than not.
        int held = (uint16_t)(prod_seen - cons);
        if (held > 0 && next_random() % 3 != 0) release(1 + next_random() % held);
    }
    release((uint16_t)(prod_seen - cons));
    dut->i_promisc = 0;
    dut->i_allmulti = 0;

    // The counts wrap at 65536 with nothing to do about it.
    {
        set_mode(0);
        Shape s;
        s.preamble = 1;
        s.gap = 2;
        Bytes f = make_frame(STATION, 60);
        while (prod_seen < 65500 && prod_seen >= 40) {
            deliver(f, STORED, "towards the wrap", s);
            release(1);
        }
        for (int i = 0; i < 80; i++) {
            deliver(make_frame(STATION, 60 + i), STORED, "across the wrap", s);
            if (i % 3 == 2) release(3);
        }
        check(prod_seen < 1000, "RX_PROD did not wrap: %u", prod_seen);
    }

    printf("eth_rx: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}

// Verilator testbench for the I2S output stage
//
// The stage drives a modelled codec that runs the link as the ADAU1761
// does when it drives BCLK and LRCLK itself: it keeps both clocks,
// samples DAC_SDATA on every BCLK rise and decodes each slot into a
// word, while a model of the audio device answers each frame request a
// set time later. The checks are the ones playback depends on: every
// frame the device delivers plays once, whole and in order, in the
// frame after its request; the data pin moves only within the stage's
// latency after a BCLK fall, at the fastest BCLK its build check allows
// as well as at the board's; a reset of the machine cuts no frame
// short, the one it lands in playing whole; and clocks that stop and
// start again skip no frame the device delivered.

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>
#include "Vaudio_i2s_out.h"

namespace {

Vaudio_i2s_out* dut = nullptr;
int pass = 0, fail = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    va_list ap; va_start(ap, fmt);
    printf("  FAIL ["); vprintf(fmt, ap); printf("]\n");
    va_end(ap); fail++;
}

// The stage's bound, in clocks, from a BCLK edge at its pin to the data
// pin or a frame request answering it.
constexpr int LATENCY   = 3;
constexpr int WORD_BITS = 16;

long now = 0;                       // simulated time, ps

double us(long t) { return t / 1e6; }
double ns(long t) { return t / 1e3; }
const char* dir(bool now_high) { return now_high ? "0->1" : "1->0"; }

struct Pair {
    uint16_t l = 0, r = 0;
    bool operator==(const Pair& o) const { return l == o.l && r == o.r; }
};

// What the device holds out of reset: no frame the sequence plays.
constexpr Pair RESET_FRAME = {0x1357, 0x9BDF};

// ── The test's clocks ────────────────────────────────────────
struct Config {
    long clk_half  = 8'333;         // ps: 60 MHz
    long bclk_half = 162'760;       // ps: 3.072 MHz, 64 BCLKs a frame at 48 kHz
    int  width     = 32;            // BCLKs a slot
    long skew      = 0;             // LRCLK's change after its BCLK fall, ps; negative is before
    long answer    = 34;            // clocks from a request to its frame; negative never answers
};
Config cfg;

long clk_period() { return 2 * cfg.clk_half; }
long bound_ps()   { return LATENCY * clk_period(); }
long frame_ps(const Config& c = cfg) { return 2L * c.width * 2 * c.bclk_half; }

// ── The device ───────────────────────────────────────────────
// Holds a frame on the stage's inputs and answers each request with the
// next of a fixed sequence: the extremes, each bit alone, then a hash.
// It shares the stage's reset, as the real device does, and in reset
// holds a frame of its own and answers nothing.
struct Device {
    struct Delivery { long t; Pair p; };

    Pair held = RESET_FRAME;
    long n = 0;                     // frames delivered
    std::deque<long> due;           // cycles at which answers land
    std::vector<Delivery> log;

    static Pair frame(long i) {
        static const Pair fixed[] = {
            {0x8000, 0x7FFF}, {0x7FFF, 0x8000}, {0x0001, 0xFFFF},
            {0xFFFF, 0x0001}, {0x5555, 0xAAAA}, {0xAAAA, 0x5555},
        };
        constexpr long FIXED = sizeof fixed / sizeof fixed[0];
        if (i < FIXED) return fixed[i];
        if (i < FIXED + WORD_BITS) {
            uint16_t b = uint16_t(1u << (i - FIXED));
            return {b, uint16_t(~b)};
        }
        uint32_t h = uint32_t(i) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return {uint16_t(h), uint16_t(h >> 16)};
    }

    void reset() { held = RESET_FRAME; due.clear(); }
    void request(long cycle) { if (cfg.answer >= 0) due.push_back(cycle + cfg.answer); }
    void tick(long cycle) {
        while (!due.empty() && due.front() <= cycle) {
            due.pop_front();
            held = frame(n++);
            log.push_back({now, held});
        }
    }
} device;

// ── The codec ────────────────────────────────────────────────
// Drives BCLK and LRCLK and samples DAC_SDATA on every BCLK rise. LRCLK
// changes on the fall that begins a slot (or skew from it, either way),
// low for a left slot. The bit sampled at a slot's first rise still
// belongs to the slot before; the slot's own positions are the rises
// after each of its falls, position 1 the MSB and the last the rise that
// begins the next slot.
struct Slot {
    long frame;                     // the codec's frame count
    bool left;
    long start = -1;                // LRCLK's change, -1 where it did not change
    long first_rise = -1;
    int  sampled = 0;               // positions sampled, of the slot's width
    uint16_t word = 0;
    int  requests = 0;              // frame requests made in it
    std::vector<long> at;           // when each position was sampled, from 1
    std::vector<bool> bit;
};

struct Codec {
    bool running = true;
    bool bclk = true, lrclk = true;
    int  pos = 0;                   // falls since the slot began, at the latest fall
    bool new_frame = false;         // the next fall begins a left slot, wherever it stands
    long next_edge = 0;
    long lr_at = -1;                // a scheduled LRCLK change
    bool lr_to = false;
    long early_start = -1;          // an LRCLK change made ahead of its fall
    long last_fall = -1, last_rise = -1;
    long frames = 0;
    std::vector<Slot> slots;

    bool next_fall_begins_slot() const { return new_frame || pos == cfg.width - 1; }
    bool next_slot_left() const { return new_frame || slots.empty() || !slots.back().left; }

    // Where the next rise samples: the slot and the position in it.
    Slot* sampling(int* p) {
        if (slots.empty()) return nullptr;
        if (pos != 0) { *p = pos; return &slots.back(); }
        *p = cfg.width;
        return slots.size() >= 2 ? &slots[slots.size() - 2] : nullptr;
    }

    void fall() {
        bclk = false; last_fall = now;
        next_edge = now + cfg.bclk_half;
        if (!next_fall_begins_slot()) { pos++; return; }
        bool left = next_slot_left();
        new_frame = false;
        pos = 0;
        if (left) frames++;
        Slot s;
        s.frame = frames; s.left = left;
        s.at.assign(cfg.width + 1, -1);
        s.bit.assign(cfg.width + 1, false);
        bool level = !left;
        if (cfg.skew < 0) {
            s.start = early_start;
        } else if (level != lrclk) {
            lr_at = now + cfg.skew; lr_to = level; s.start = lr_at;
        }
        slots.push_back(s);
    }

    void rise() {
        bclk = true; last_rise = now;
        next_edge = now + cfg.bclk_half;
        if (pos == 0 && !slots.empty()) slots.back().first_rise = now;
        int p;
        if (Slot* s = sampling(&p)) {
            bool b = dut->o_sdata;
            s->at[p] = now; s->bit[p] = b; s->sampled++;
            if (p <= WORD_BITS && b) s->word |= uint16_t(1u << (WORD_BITS - p));
        }
        if (cfg.skew < 0 && next_fall_begins_slot()) {
            bool level = !next_slot_left();
            early_start = -1;
            if (level != lrclk) {
                lr_at = now + cfg.bclk_half + cfg.skew; lr_to = level;
                early_start = lr_at;
            }
        }
    }

    // Starts the clocks again on a new frame, as a codec does whose own
    // clock stopped: the next fall begins a left slot.
    void restart() {
        running = true; new_frame = true;
        next_edge = now + cfg.bclk_half;
    }

    // Frame position (0 to two slots) at the latest fall. The codec
    // begins part way through a right slot, before its first record.
    int frame_pos() const {
        bool left = !slots.empty() && slots.back().left;
        return (left ? 0 : cfg.width) + pos;
    }

    // When the next fall at frame position fp comes, the clocks running
    // on as they are.
    long next_fall_at(int fp) const {
        long t = bclk ? next_edge : next_edge + cfg.bclk_half;
        int  at = (frame_pos() + 1) % (2 * cfg.width);
        int  steps = ((fp - at) % (2 * cfg.width) + 2 * cfg.width) % (2 * cfg.width);
        return t + steps * 2 * cfg.bclk_half;
    }
} codec;

std::string slot_name(const Slot* s) {
    if (!s) return "no slot";
    return "frame " + std::to_string(s->frame) + "'s " + (s->left ? "left" : "right") + " slot";
}

std::string describe(const Slot* s, int p) {
    if (!s) return "before the codec's first slot";
    char buf[96];
    const char* side = s->left ? "left" : "right";
    if (p <= WORD_BITS)
        snprintf(buf, sizeof buf, "frame %ld's %s word, bit %d", s->frame, side, WORD_BITS - p);
    else
        snprintf(buf, sizeof buf, "frame %ld's %s slot, padding position %d", s->frame, side, p);
    return buf;
}

// ── The watch on the stage's outputs ─────────────────────────
struct Watch {
    int  reports = 0;
    long worst_delay = 0;           // a data change after its BCLK fall
    long worst_margin = LONG_MAX;   // a data change ahead of the next rise
    long requests = 0;

    void report(const char* fmt, ...) {
        if (reports++ >= 8) return;
        va_list ap; va_start(ap, fmt);
        printf("  "); vprintf(fmt, ap); printf("\n");
        va_end(ap);
    }

    void data(bool v) {
        int p = 0;
        Slot* s = codec.sampling(&p);
        long after = now - codec.last_fall;
        bool low = codec.last_fall > codec.last_rise;
        if (!low || codec.last_fall < 0 || after > bound_ps()) {
            report("timing: DAC_SDATA %s at %.4f us, %.1f ns after BCLK fell at %.4f us "
                   "(bound %.1f ns), BCLK now %s, carrying %s",
                   dir(v), us(now), ns(after), us(codec.last_fall), ns(bound_ps()),
                   low ? "low" : "high", describe(s, p).c_str());
            return;
        }
        worst_delay  = std::max(worst_delay, after);
        worst_margin = std::min(worst_margin, codec.last_fall + cfg.bclk_half - now);
    }

    void request(bool held_over) {
        requests++;
        if (held_over) report("request: held high for a second cycle at %.4f us", us(now));
        Slot* s = codec.slots.empty() ? nullptr : &codec.slots.back();
        if (!s || !s->left || s->first_rise < 0) {
            report("request: at %.4f us in %s, which begins no frame", us(now),
                   slot_name(s).c_str());
            return;
        }
        s->requests++;
        long after = now - s->first_rise;
        if (after <= 0 || after > bound_ps())
            report("request: at %.4f us, %.1f ns after frame %ld's left slot began "
                   "at its first BCLK rise (bound %.1f ns)",
                   us(now), ns(after), s->frame, ns(bound_ps()));
    }
} watch;

// ── The clock and the pins ───────────────────────────────────
bool clk = false, rst = false;
long next_clk = 0, cycle = 0;
bool sdata_was = false, req_was = false, rst_was = false;
long asserted_at = 0;               // the edge the stage last entered reset on
long released_at = -1;              // and the edge it last left it on

void clk_edge() {
    clk = !clk;
    next_clk = now + cfg.clk_half;
    dut->i_clk = clk; dut->eval();
    if (!clk) return;
    cycle++;
    bool in_rst = dut->i_rst;       // as this edge sampled it
    if (!rst_was && in_rst) asserted_at = now;
    if (rst_was && !in_rst) released_at = now;
    if (dut->o_sdata != sdata_was) watch.data(dut->o_sdata);
    if (dut->o_frame_req) {
        if (in_rst) watch.report("request: at %.4f us, during reset", us(now));
        watch.request(req_was);
        if (!in_rst) device.request(cycle);
    }
    sdata_was = dut->o_sdata; req_was = dut->o_frame_req; rst_was = in_rst;
    if (in_rst) device.reset(); else device.tick(cycle);
    dut->i_frame_l = device.held.l; dut->i_frame_r = device.held.r;
    dut->i_rst = rst;
}

void step() {
    long t_codec = codec.running ? codec.next_edge : LONG_MAX;
    long t_lr    = codec.lr_at >= 0 ? codec.lr_at : LONG_MAX;
    now = std::min({next_clk, t_codec, t_lr});
    if (now == t_lr) {
        codec.lrclk = codec.lr_to; codec.lr_at = -1;
    } else if (now == t_codec) {
        if (codec.bclk) codec.fall(); else codec.rise();
    } else {
        clk_edge();
        return;
    }
    dut->i_bclk = codec.bclk; dut->i_lrclk = codec.lrclk; dut->eval();
}

void run_until(long t) { while (now < t) step(); }
void run_for(long ps)  { run_until(now + ps); }

// Reset asserted from the next edge until time `t`: asserted_at and
// released_at name the edges the stage saw it begin and end on.
void reset_until(long t) {
    rst = true;
    run_until(t);
    rst = false;
    long released = released_at;
    while (released_at == released) step();
}

// A fresh stage and codec, the codec part way through a right slot.
void begin(const Config& c) {
    delete dut;
    dut = new Vaudio_i2s_out;
    cfg = c;
    now = 0; cycle = 0; clk = false; next_clk = cfg.clk_half;
    rst = true; sdata_was = false; req_was = false; rst_was = true;
    asserted_at = 0; released_at = -1;
    codec = Codec{};
    codec.pos = cfg.width / 3; codec.next_edge = cfg.bclk_half / 3;
    device = Device{};
    watch = Watch{};
    dut->i_rst = 1; dut->i_clk = 0;
    dut->i_bclk = codec.bclk; dut->i_lrclk = codec.lrclk;
    dut->i_frame_l = device.held.l; dut->i_frame_r = device.held.r;
    dut->eval();
    reset_until(4 * clk_period());
}

// ── The frames played ────────────────────────────────────────
// The frames whose left slot had its first rise between `from` and `to`
// have to be what the device held at `from` and then what it delivered
// before `to`, one each and in order, with `requests` made in each;
// once the deliveries run out, the last repeats. A frame begun in the
// window counts wherever it ends: one still playing as the window
// closes has to come out whole all the same. A slot whose LRCLK did not
// change begins nothing the stage can see, so neither does its frame.
void check_frames(const char* what, long from, long to, Pair first, size_t min_frames,
                  int requests = 1) {
    struct Want { Pair p; long t; };
    std::vector<Want> want = {{first, -1}};
    for (const auto& d : device.log)
        if (d.t > from && d.t < to) want.push_back({d.p, d.t});

    size_t frames = 0;
    int    shown = 0;
    const auto& sl = codec.slots;
    for (size_t i = 0; i + 1 < sl.size(); i++) {
        const Slot& L = sl[i];
        const Slot& R = sl[i + 1];
        if (!L.left || L.start < 0 || R.left || R.start < 0) continue;
        if (L.first_rise <= from || L.first_rise >= to) continue;
        if (L.sampled != cfg.width || R.sampled != cfg.width) continue;

        const Want& w = want[std::min(frames, want.size() - 1)];
        frames++;
        bool padding = true;
        for (const Slot* s : {&L, &R})
            for (int p = WORD_BITS + 1; p <= cfg.width; p++) padding = padding && !s->bit[p];
        bool ok = L.word == w.p.l && R.word == w.p.r && padding &&
                  L.requests == requests && R.requests == 0;
        if (!ok && shown++ < 6) {
            char from_what[64], closing[64] = "";
            if (w.t < 0) snprintf(from_what, sizeof from_what, "the frame held at %.4f us", us(from));
            else         snprintf(from_what, sizeof from_what, "the frame delivered at %.4f us", us(w.t));
            if (R.at[cfg.width] > to)
                snprintf(closing, sizeof closing, ", still playing as the window closed at %.4f us",
                         us(to));
            printf("  %s: frame %ld (%.4f to %.4f us%s) played L=%04X R=%04X, expected "
                   "L=%04X R=%04X, %s; %s, %d request%s in its left slot (expected %d), "
                   "%d in its right\n",
                   what, L.frame, us(L.start), us(R.at[cfg.width]), closing, L.word, R.word,
                   w.p.l, w.p.r, from_what, padding ? "padding clear" : "padding set",
                   L.requests, L.requests == 1 ? "" : "s", requests, R.requests);
        }
        check(ok, "%s: frame %ld played out of order or garbled", what, L.frame);
    }
    check(frames >= min_frames, "%s: %zu whole frames played, expected at least %zu",
          what, frames, min_frames);
}

void check_timing(const char* what) {
    check(watch.reports == 0, "%s: %d timing or request reports", what, watch.reports);
}

void summary(const char* what) {
    printf("  %-28s data %.1f ns after a fall at worst, %.1f ns ahead of the rise\n",
           what, ns(watch.worst_delay), ns(watch.worst_margin));
}

// One test: playback from a fresh reset, frames checked from the release.
void playback(const char* what, const Config& c, size_t frames) {
    begin(c);
    long from = released_at;
    run_for(long(frames + 2) * frame_ps());
    check_frames(what, from, now, RESET_FRAME, frames);
    check_timing(what);
    summary(what);
}

} // namespace

int main() {
    const Config board;

    // ── The board's rates ────────────────────────────────────
    playback("board rates", board, 60);

    // An answer just short of a frame period is still in time: the
    // request comes one frame ahead.
    {
        Config c = board;
        c.answer = frame_ps(c) / (2 * c.clk_half) - 4;
        playback("answers a frame late", c, 30);
    }

    // A device that delivers nothing leaves the held frame playing, and
    // the requests keep coming, one a frame.
    {
        Config c = board;
        c.answer = -1;
        playback("no answers", c, 20);
        check(watch.requests >= 20, "no answers: %ld requests", watch.requests);
    }

    // LRCLK is read at BCLK rises, half a BCLK from either of its
    // changes, so its skew from the fall is no matter.
    {
        Config c = board;
        c.skew = 30'000;
        playback("LRCLK 30 ns late", c, 20);
        c.skew = -30'000;
        playback("LRCLK 30 ns early", c, 20);
    }

    // Slots as narrow as the word: the LSB goes at the next slot's
    // first rise, and no padding.
    {
        Config c = board;
        c.width = 16;
        c.bclk_half *= 2;
        playback("16-BCLK slots", c, 30);
    }

    // The fastest BCLK the build check allows: half a BCLK is LATENCY
    // clocks and one more.
    {
        Config c = board;
        c.bclk_half = (LATENCY + 1) * 2 * c.clk_half + 37;
        playback("BCLK at the build's limit", c, 60);
    }

    // ── A reset of the machine ───────────────────────────────
    // The bus holds reset for 10 ms at least, hundreds of frames, and
    // the codec plays on through it. What reaches the codec depends only
    // on where in a frame the reset begins and where it ends, so two
    // frames and a part stand in for it, begun a quarter BCLK past a
    // fall at every fifth position of the frame. The frame a reset lands
    // in plays whole; the frames inside it are the device's own in
    // reset, with no request made; and the frames after go on in order.
    {
        struct Window { std::string what; long from, to; int requests; };
        std::vector<Window> windows;
        const long span = 2 * frame_ps(board) + 37 * 2 * board.bclk_half;
        begin(board);
        long from = released_at;
        for (int fp = 0; fp < 2 * board.width; fp += 5) {
            long at = codec.next_fall_at(fp) + board.bclk_half / 2;
            while (at < now + 3 * frame_ps()) at += frame_ps();
            run_until(at);
            reset_until(at + span);
            std::string name = "reset at position " + std::to_string(fp);
            windows.push_back({name + ", frames up to it", from, asserted_at, 1});
            windows.push_back({name + ", frames inside it", asserted_at, released_at, 0});
            from = released_at;
        }
        run_for(4 * frame_ps());
        windows.push_back({"after the last reset", from, now, 1});
        for (const Window& w : windows)
            check_frames(w.what.c_str(), w.from, w.to, RESET_FRAME, 2, w.requests);
        check_timing("resets");
        summary("resets");
    }

    // ── Clocks that stop and start again ─────────────────────
    // As the codec's do while its setup restarts it: stopped part way
    // through a left slot and then a right one, they come back on a new
    // frame. The slot they stopped in may carry anything; the frames
    // after go on in order, the one the device holds first.
    for (int fp : {10, board.width + 10}) {
        char what[64];
        snprintf(what, sizeof what, "clocks stopped at %d", fp);
        begin(board);
        run_for(3 * frame_ps());
        run_until(codec.next_fall_at(fp));
        while (codec.last_fall != now) step();
        codec.running = false;
        long requests = watch.requests;
        run_for(20'000'000);
        check(watch.requests == requests, "%s: %ld requests with the clocks stopped",
              what, watch.requests - requests);
        long from = now;
        Pair held = device.held;
        codec.restart();
        run_for(10 * frame_ps());
        check_frames(what, from, now, held, 8);
        check_timing(what);
        summary(what);
    }

    printf("\naudio_i2s_out: %d/%d tests passed\n", pass, pass + fail);
    delete dut;
    return fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

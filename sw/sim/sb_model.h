// sb_model.h — shadow store buffers on a cycle estimate (+sbmodel).
//
// gen3 plans a store buffer between the core and the uncore
// (doc/internals/penumbra3/memory-performance.md, "Store buffer at the
// core boundary"): a cacheable store completes when it enters the
// buffer, and the buffer drains through the uncore in the background.
// What it costs is time — the core waiting for a free entry, and an
// access that has to reach L2 waiting for entries ahead of it — so
// unlike the cache and TLB models, this one keeps a clock.
//
// The clock is an estimate.  Every instruction costs a fixed number of
// cycles (+sbmodel_cpi=, 1 by default), every L1 fill the cycles of its
// source, every uncached access UNCACHED_ACCESS, and the store buffer's
// waits come on top.  Nothing else a pipeline stalls on is modelled,
// which packs the stream tighter than hardware would: the waits read as
// an upper bound.  What served each fetch and load comes from the first
// hierarchy +cachemodel lists, so the two models run together.
//
// The uncore has an L2 and a bus, and never abandons a transfer it has
// started on the bus.  Without posting, a drain holds both for its whole
// cost, as does every fill and uncached access: one thing at a time.
// With posting ("/post"), L2 takes an entry's write in POSTED_L2_WRITE
// cycles and the entry then waits, in order, for its turn on the bus; a
// fill L2 serves needs only L2, while a fill from memory and an uncached
// access need both.  A write that has not reached the bus yields to a
// waiting access, unless the access has to wait for that entry.  Each
// buffer:
//
//  - holds line entries (a line and a byte mask, which stores to that
//    line merge into) or word entries (one store each);
//  - takes a store into the entry of its line that has not started
//    draining, else into a new entry, the core waiting for the oldest
//    entry to finish draining when every entry is taken;
//  - holds its youngest line entry open until a store to another line
//    arrives, the line is complete, or an access has to wait for it;
//  - drains oldest first, each entry as one burst over the words from
//    the first it wrote to the last, at a cost set per burst length,
//    and frees the entry once the bus write ends;
//  - makes an L1 fill wait by its policy — "all" for every entry,
//    "line" for the entries up to the youngest holding the fill's line,
//    "merge" for none, the fill taking any buffered bytes of its line
//    from the buffer — and an uncached access wait for every entry,
//    which keeps it ordered after every earlier store.  With posting, a
//    fill L2 serves waits only for those entries' L2 writes; the others
//    wait for their bus writes, since memory must hold the data first.
//
// Only a cacheable store that the enabled L1-D takes enters a buffer;
// any other store is an uncached access.  The clocks and buffers run
// throughout; what they count, they count inside the measurement window.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "cache_model.h"

// Estimated costs in CPU cycles at 37.5 MHz: an L1 fill over gen3's
// line-wide L2 transfer, from L2 and from memory
// (memory-performance.md, "The line-wide transfer"), and a single-beat
// transaction through penumbra3_bus_master (measured, "Sustained writes
// on the SDRAM stack").
static constexpr double FILL_FROM_L2     = 5;
static constexpr double FILL_FROM_MEMORY = 24;
static constexpr double UNCACHED_ACCESS  = 5;

// Cycles the L2 pipeline takes to accept a line write when write-through
// is posted: an estimate until the gen3 L2 exists.
static constexpr double POSTED_L2_WRITE  = 2;

// Drain costs by burst length, one to four words, in CPU cycles at
// 37.5 MHz through penumbra3_bus_master.  "bus" is the SDRAM stack as it
// stands: each word its own controller request, paced by the CDC's two
// slots, which reproduces the measured 5 cycles a word alone and 13 a
// line.  "combine" is the adapter gathering a burst into one request, so
// the bus's two cycles a word pace it (memory-performance.md, "SDRAM
// side: write combining").
struct DrainPreset { const char* name; uint32_t cycles[4]; };
static constexpr DrainPreset DRAIN_PRESETS[] = {
    { "bus",     { 5, 7, 11, 13 } },
    { "combine", { 5, 7,  9, 11 } },
};

enum class SbPolicy { All, Line, Merge };

struct SbConfig {
    std::string label;
    uint32_t    entries;
    bool        lines;       // line entries that merge, else word entries
    SbPolicy    policy;
    uint32_t    drain[4];    // cycles to drain a burst of 1..4 words
    bool        posted;      // L2 posts write-through to the bus
};

// ── One store buffer on its own clock ────────────────────────────
class StoreBuffer {
public:
    explicit StoreBuffer(SbConfig config) : cfg_(std::move(config)) {}

    // An instruction fetch: its fill if L1-I missed, then the
    // instruction's own cycles.
    void fetch(Served served, uint32_t pa, double cpi) {
        if (served != Served::L1) reach_uncore(fill_or_uncached(Access::I, served), served, pa);
        now_ += cpi;
    }

    void load(Served served, uint32_t pa) {
        if (served != Served::L1) reach_uncore(fill_or_uncached(Access::D, served), served, pa);
    }

    void store(uint32_t pa, uint32_t bytes, bool buffered) {
        if (!buffered) {
            counts_.uncached_stores++;
            reach_uncore(Access::Uncached, Served::Uncached, pa);
            return;
        }
        advance(now_);
        counts_.stores++;
        uint32_t line = pa / LINE_BYTES;
        uint16_t mask = uint16_t(((1u << bytes) - 1) << (pa % LINE_BYTES));
        if (cfg_.lines) {
            for (Entry& e : queue_)
                if (!e.l2_started && e.line == line) {
                    e.mask |= mask;
                    counts_.merged++;
                    if (e.mask == FULL_LINE) close(e);
                    return;
                }
            // A store to another line closes the youngest entry.
            if (!queue_.empty()) close(queue_.back());
        }
        if (queue_.size() == cfg_.entries) {
            double freed = finish_through(1, true);
            if (freed > now_) {
                counts_.waits[int(Access::Full)] += freed - now_;
                counts_.waited[int(Access::Full)]++;
                now_ = freed;
            }
            advance(now_);
        }
        Entry e;
        e.line = line;
        e.mask = mask;
        e.open = cfg_.lines && mask != FULL_LINE;
        e.ready_at = now_;
        queue_.push_back(e);
        counts_.entries++;
    }

    void clear_counts() {
        counts_ = Counts();
        window_start_ = now_;
    }

    void report(FILE* out, uint64_t insns) const {
        const char* policy = cfg_.policy == SbPolicy::All  ? "every entry"
                           : cfg_.policy == SbPolicy::Line ? "entries up to their line"
                           :                                 "no entry, merging";
        fprintf(out, "-- store buffer %s: %u %s entries, fills wait for %s, drain %u/%u/%u/%u cycles%s\n",
                cfg_.label.c_str(), cfg_.entries, cfg_.lines ? "line" : "word", policy,
                cfg_.drain[0], cfg_.drain[1], cfg_.drain[2], cfg_.drain[3],
                cfg_.posted ? ", write-through posted" : "");
        const Counts& c = counts_;
        fprintf(out, "  stores: %llu buffered, %llu merged, %llu entries; %llu uncached\n",
                (unsigned long long)c.stores, (unsigned long long)c.merged,
                (unsigned long long)c.entries, (unsigned long long)c.uncached_stores);
        fprintf(out, "  entries drained by words: %llu %llu %llu %llu\n",
                (unsigned long long)c.drained[0], (unsigned long long)c.drained[1],
                (unsigned long long)c.drained[2], (unsigned long long)c.drained[3]);
        fprintf(out, "  waits, cycles: buffer full %.0f, I fills %.0f, D fills %.0f, uncached %.0f\n",
                c.waits[int(Access::Full)], c.waits[int(Access::I)],
                c.waits[int(Access::D)], c.waits[int(Access::Uncached)]);
        fprintf(out, "  accesses that waited: buffer full %llu, I fills %llu of %llu, D fills %llu of %llu,"
                     " uncached %llu of %llu\n",
                (unsigned long long)c.waited[int(Access::Full)],
                (unsigned long long)c.waited[int(Access::I)], (unsigned long long)c.reached[int(Access::I)],
                (unsigned long long)c.waited[int(Access::D)], (unsigned long long)c.reached[int(Access::D)],
                (unsigned long long)c.waited[int(Access::Uncached)],
                (unsigned long long)c.reached[int(Access::Uncached)]);
        double k = insns / 1000.0;
        fprintf(out, "  per 1000 instructions: buffer full %.2f, I fills %.2f, D fills %.2f, uncached %.2f;"
                     " model cycles %.1f\n",
                k ? c.waits[int(Access::Full)] / k : 0.0, k ? c.waits[int(Access::I)] / k : 0.0,
                k ? c.waits[int(Access::D)] / k : 0.0, k ? c.waits[int(Access::Uncached)] / k : 0.0,
                k ? (now_ - window_start_) / k : 0.0);
    }

private:
    static constexpr uint32_t LINE_BYTES = 16;
    static constexpr uint16_t FULL_LINE  = 0xFFFF;

    enum class Access { I = 0, D = 1, Uncached = 2, Full = 3 };

    struct Entry {
        uint32_t line = 0;
        uint16_t mask = 0;           // bytes written, bit per byte of the line
        bool     open = false;       // the youngest line entry, still merging
        bool     l2_started = false; // draining: no longer merges
        bool     bus_started = false;
        double   ready_at = 0;       // when it could start draining
        double   l2_start = 0, l2_done = 0;
        double   done_at = 0;        // the bus write's end, which frees it
    };

    struct Counts {
        uint64_t stores = 0, merged = 0, entries = 0, uncached_stores = 0;
        uint64_t drained[4] = {};                 // by burst length in words
        double   waits[4] = {};                   // by Access, in cycles
        uint64_t waited[4] = {}, reached[4] = {}; // accesses that waited, and all
    };

    static Access fill_or_uncached(Access fill, Served served) {
        return served == Served::Uncached ? Access::Uncached : fill;
    }

    // An access that needs the uncore: it waits for the entries it has
    // to, then for whatever is in flight on what it uses, and then holds
    // that for its own cycles.
    void reach_uncore(Access kind, Served served, uint32_t pa) {
        advance(now_);
        size_t n = kind == Access::Uncached ? queue_.size() : must_drain(pa / LINE_BYTES);
        bool bus = !cfg_.posted || served != Served::L2;
        double ready = n ? finish_through(n, bus) : now_;
        double start = std::max({now_, ready, l2_free_, bus ? bus_free_ : 0.0});
        counts_.reached[int(kind)]++;
        if (start > now_) {
            counts_.waits[int(kind)] += start - now_;
            counts_.waited[int(kind)]++;
        }
        now_ = start + (served == Served::L2       ? FILL_FROM_L2
                      : served == Served::Memory   ? FILL_FROM_MEMORY
                      :                              UNCACHED_ACCESS);
        l2_free_ = now_;
        if (bus) bus_free_ = now_;
        advance(now_);
    }

    // How many entries, oldest first, must finish draining before an L1
    // fill of `line` may use the uncore.
    size_t must_drain(uint32_t line) const {
        switch (cfg_.policy) {
        case SbPolicy::All:
            return queue_.size();

        case SbPolicy::Merge:
            return 0;

        case SbPolicy::Line: {
            auto pred = [line](Entry e) { return e.line == line; };
            auto i = std::find_if(queue_.rbegin(), queue_.rend(), pred);
            if (i != queue_.rend())
                return std::distance(queue_.begin(), i.base());
            else
                return 0;
        }

        default:
            fprintf(stderr, "Unsupported store buffer drain policy");
            abort();
        }
    }

    void close(Entry& e) {
        if (e.open) {
            e.open = false;
            e.ready_at = now_;
        }
    }

    // Start the writes that could have started by `t`, oldest first,
    // and retire the entries whose bus writes ended by then.
    void advance(double t) {
        for (Entry& e : queue_) {
            if (e.l2_started) continue;
            if (e.open) break;
            double start = std::max(l2_free_, e.ready_at);
            if (start > t) break;
            begin_l2(e, start);
        }
        if (cfg_.posted)
            for (Entry& e : queue_) {
                if (e.bus_started) continue;
                if (!e.l2_started || std::max(bus_free_, e.l2_start) > t) break;
                begin_bus(e);
            }
        while (!queue_.empty() && queue_.front().bus_started && queue_.front().done_at <= t)
            retire_oldest();
    }

    // Start what the oldest `n` entries still need -- their L2 writes,
    // and with `bus` their bus writes -- and return when the last of
    // those ends.
    double finish_through(size_t n, bool bus) {
        for (size_t i = 0; i < n; i++) {
            Entry& e = queue_[i];
            close(e);
            if (!e.l2_started) begin_l2(e, std::max(l2_free_, e.ready_at));
        }
        if (bus && cfg_.posted)
            for (size_t i = 0; i < n; i++)
                if (!queue_[i].bus_started) begin_bus(queue_[i]);
        const Entry& last = queue_[n - 1];
        return bus ? last.done_at : last.l2_done;
    }

    // Without posting the whole drain holds L2 and the bus together.
    void begin_l2(Entry& e, double start) {
        e.l2_started = true;
        e.l2_start = start;
        if (!cfg_.posted) {
            e.bus_started = true;
            e.l2_done = e.done_at = start + drain_cycles(e);
            l2_free_ = bus_free_ = e.done_at;
            return;
        }
        e.l2_done = start + POSTED_L2_WRITE;
        l2_free_ = e.l2_done;
    }

    void begin_bus(Entry& e) {
        e.bus_started = true;
        e.done_at = std::max(bus_free_, e.l2_start) + drain_cycles(e);
        bus_free_ = e.done_at;
    }

    double drain_cycles(const Entry& e) const { return cfg_.drain[burst_words(e.mask) - 1]; }

    void retire_oldest() {
        counts_.drained[burst_words(queue_.front().mask) - 1]++;
        queue_.pop_front();
    }

    // The words a drain carries: from the first the entry wrote to the last.
    static uint32_t burst_words(uint16_t mask) {
        uint32_t first = __builtin_ctz(mask) / 4;
        uint32_t last  = (31 - __builtin_clz(mask)) / 4;
        return last - first + 1;
    }

    SbConfig          cfg_;
    std::deque<Entry> queue_;
    double            now_ = 0, l2_free_ = 0, bus_free_ = 0, window_start_ = 0;
    Counts            counts_;
};

// ── The model: every buffer +sbmodel names ───────────────────────
class SbModel {
public:
    // Parse the +sbmodel value: empty for a default comparison, or a
    // comma-separated list of ENTRIES{l|w}/POLICY/DRAIN[/post] — ENTRIES
    // line or word entries, POLICY "all", "line" or "merge", DRAIN a preset
    // name or four colon-separated cycle counts for bursts of 1..4 words,
    // and "/post" for write-through posted past L2.
    static bool parse(const char* spec, double cpi, SbModel& model, std::string& err) {
        model.cpi_ = cpi;
        std::string s(*spec ? spec : "8l/all/bus,8l/line/bus,8l/all/combine,8l/line/combine,8w/all/bus");
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            SbConfig config;
            if (!parse_item(item, config, err)) {
                err = "'" + item + "': " + err;
                return false;
            }
            model.buffers_.emplace_back(config);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return true;
    }

    void fetch(Served s, uint32_t pa) { for (auto& b : buffers_) b.fetch(s, pa, cpi_); }
    void load(Served s, uint32_t pa)  { for (auto& b : buffers_) b.load(s, pa); }
    void store(uint32_t pa, uint32_t bytes, bool buffered) {
        for (auto& b : buffers_) b.store(pa, bytes, buffered);
    }
    void clear_counts() { for (auto& b : buffers_) b.clear_counts(); }
    void report(FILE* out, uint64_t insns) const { for (const auto& b : buffers_) b.report(out, insns); }

private:
    static bool parse_item(const std::string& item, SbConfig& c, std::string& err) {
        c.label = item;
        size_t a = item.find('/'), b = a == std::string::npos ? a : item.find('/', a + 1);
        if (b == std::string::npos) { err = "not ENTRIES{l|w}/POLICY/DRAIN[/post]"; return false; }
        size_t o = item.find('/', b + 1);
        std::string entries = item.substr(0, a), policy = item.substr(a + 1, b - a - 1),
                    drain = item.substr(b + 1, o == std::string::npos ? o : o - b - 1);
        c.posted = o != std::string::npos;
        if (c.posted && item.substr(o + 1) != "post") {
            err = "unknown option '/" + item.substr(o + 1) + "' (post)";
            return false;
        }

        char* end = nullptr;
        unsigned long n = strtoul(entries.c_str(), &end, 10);
        if (n < 1 || n > 64 || (*end != 'l' && *end != 'w') || end[1]) {
            err = "entries must be 1..64 followed by l (line) or w (word)";
            return false;
        }
        c.entries = uint32_t(n);
        c.lines = *end == 'l';

        if (policy == "all")       c.policy = SbPolicy::All;
        else if (policy == "line")  c.policy = SbPolicy::Line;
        else if (policy == "merge") c.policy = SbPolicy::Merge;
        else { err = "unknown policy '" + policy + "' (all, line, merge)"; return false; }

        for (const DrainPreset& p : DRAIN_PRESETS)
            if (drain == p.name) {
                std::copy(p.cycles, p.cycles + 4, c.drain);
                return true;
            }
        const char* d = drain.c_str();
        for (int i = 0; i < 4; i++) {
            c.drain[i] = uint32_t(strtoul(d, &end, 10));
            if (end == d || *end != (i < 3 ? ':' : '\0')) {
                err = "drain '" + drain + "' is neither a preset (bus, combine) nor four cycle counts";
                return false;
            }
            d = end + 1;
        }
        return true;
    }

    double cpi_ = 1;
    std::vector<StoreBuffer> buffers_;
};

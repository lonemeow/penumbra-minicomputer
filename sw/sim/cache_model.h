// cache_model.h — shadow cache hierarchies for the ISS (+cachemodel).
//
// The ISS executes architecturally and holds no cache contents.  A cache
// model watches the accesses it makes — every fetch, load and store, by
// physical address and the translating PTE's C bit — and runs them
// through one or more cache hierarchies side by side, counting what each
// would have hit and missed.  It counts what the hardware's perfctrs
// count, plus the one split they fold together: which L1 fills L2
// served and which went to memory, for instructions and data.
//
// Each level follows the RTL's behaviour:
//
//  - L1-I and L1-D (cache_bram_vipt; gen1's cache_vipt is the
//    direct-mapped case): a read miss fills the line from L2; stores
//    write through without allocating and update the local copy on a
//    hit.  The victim is the first invalid way, else the tree-PLRU way;
//    read hits, write hits and fills touch it.
//  - L2 (l2_cache): fed by L1 fills and write-throughs, write-no-allocate
//    with update on a hit.  The victim is the tree-PLRU way whether or
//    not an invalid way exists; read hits and fills touch it, write hits
//    do not.  Reads count per fill word, as its perfctr does: a fill L2
//    serves is a read hit per word, one it misses is one read miss and a
//    read hit for each remaining word.
//  - A level caches only while its CTRL enable is set and the access is
//    cacheable.  A disabled L1 forwards reads to L2 as uncacheable; a
//    write-through carries the PTE's C bit whatever L1 did with it.
//
// Beside each L1-I runs a next-line prediction buffer that no generation
// builds (doc/internals/penumbra3/memory-performance.md, "Next-line
// prediction buffer for L1-I"), counting which L1-I misses it would have
// served.  An L1-I miss to line N looks N+1 up in L2 and holds it if L2
// has it and it lies in the same page; a later L1-I miss to the held line
// is served by the buffer.  Two policies run side by side: "once" looks
// up the next line only after a miss the buffer did not serve, "continue"
// after every miss.  The buffer never changes what the caches hold or
// count, and an L1-I INVAL_ALL clears it.
//
// Each L1-D load miss is also classified by the instruction after it in
// program order: a load or store to the set the line goes into, one to
// another set, or no access at all.  On a pipelined core that instruction
// has launched its L1-D lookup as the miss requests its line, and a lookup
// of the filled set is held across the install
// (doc/internals/penumbra3/l1-cache.md, "Busy and done"), so a core that
// resumes as the line arrives still waits for it; the other two need not.
// No bubble is modelled between the two instructions.
//
// The model sees the architectural stream only: no wrong-path fetches,
// and nothing that needs time — fill latency, prefetch, overlap.  Its
// counts are exact for that stream; cycles are not modelled.  What
// served each fetch and load is returned for the store-buffer model
// (sb_model.h), which does keep a clock.  It indexes
// every level by physical address, which for a VIPT L1 matches the
// hardware only while a way fits in a page (4 KiB); a larger way is
// modelled as if physically indexed.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "penumbra_cores.h"

// ── Tree-PLRU ────────────────────────────────────────────────────
// A binary tree over the ways (a power of two, at most 64), numbered as a
// heap: node 0 is the root, the ways - 1 inner nodes come first, and the
// ways themselves are the leaves after them, left to right.  Each inner
// node holds one bit saying which side its least-recently-used way is on:
// 0 left, 1 right.  For four ways:
//
//                   node 0
//            ┌─────────┴─────────┐
//         node 1              node 2
//        ┌───┴───┐           ┌───┴───┐
//       (3)     (4)         (5)     (6)
//      way 0   way 1       way 2   way 3
//
// so the inner nodes' bits are the RTL's plru[2:0].

static inline uint32_t plru_left(uint32_t node)   { return 2 * node + 1; }
static inline uint32_t plru_right(uint32_t node)  { return 2 * node + 2; }
static inline uint32_t plru_parent(uint32_t node) { return (node - 1) / 2; }
static inline bool     plru_is_left(uint32_t node) { return node & 1; }

static inline bool plru_is_inner(uint32_t node, uint32_t ways) { return node < ways - 1; }
static inline uint32_t plru_leaf(uint32_t way, uint32_t ways)  { return way + ways - 1; }
static inline uint32_t plru_way(uint32_t leaf, uint32_t ways)  { return leaf - (ways - 1); }

static inline bool plru_points_right(uint64_t bits, uint32_t node) { return (bits >> node) & 1; }

// The victim: from the root, follow each node's bit down to a way.
static inline uint32_t plru_victim(uint64_t bits, uint32_t ways) {
    uint32_t node = 0;
    while (plru_is_inner(node, ways))
        node = plru_points_right(bits, node) ? plru_right(node) : plru_left(node);
    return plru_way(node, ways);
}

// Using `way`: from its leaf up to the root, set each node to point at the
// child the path did not come through.  Set, not toggle -- a node may
// already point away (the way was used recently), and toggling it would
// point it back at the way just used.
static inline uint64_t plru_touch(uint64_t bits, uint32_t ways, uint32_t way) {
    for (uint32_t node = plru_leaf(way, ways); node != 0; node = plru_parent(node)) {
        uint64_t parent_bit = uint64_t(1) << plru_parent(node);
        if (plru_is_left(node)) bits |= parent_bit;
        else                    bits &= ~parent_bit;
    }
    return bits;
}

// ── One cache ────────────────────────────────────────────────────
class ModelCache {
public:
    // How a fill picks its way: an invalid one first (the L1s), or
    // whatever the tree says (the L2).
    enum class Victim { InvalidFirst, TreeOnly };

    ModelCache(const CacheGeometry& g, Victim victim, bool touch_write_hits)
        : sets_(g.sets), ways_(g.ways), line_shift_(__builtin_ctz(g.line_bytes)),
          victim_(victim), touch_write_hits_(touch_write_hits),
          line_(g.sets * g.ways, 0), valid_(g.sets * g.ways, 0), plru_(g.sets, 0) {}

    uint32_t line_of(uint32_t pa) const { return pa >> line_shift_; }

    // A read: true on a hit, which touches the way.  A miss changes
    // nothing; the caller fills once the line arrives.
    bool read(uint32_t line) {
        int w = find(line);
        if (w < 0) return false;
        touch(line, w);
        return true;
    }

    // A lookup that leaves the replacement state alone.
    bool contains(uint32_t line) const { return find(line) >= 0; }

    // A write-through: true on a hit.  Never allocates.
    bool write(uint32_t line) {
        int w = find(line);
        if (w < 0) return false;
        if (touch_write_hits_) touch(line, w);
        return true;
    }

    void fill(uint32_t line) {
        uint32_t set = line & (sets_ - 1);
        uint32_t way = plru_victim(plru_[set], ways_);
        if (victim_ == Victim::InvalidFirst) {
            for (uint32_t w = 0; w < ways_; w++)
                if (!valid_[set * ways_ + w]) { way = w; break; }
        }
        line_[set * ways_ + way]  = line;
        valid_[set * ways_ + way] = 1;
        touch(line, way);
    }

    // INVAL_ALL drops every line; the replacement state is left as is, as
    // the RTL leaves it.
    void invalidate_all() { std::fill(valid_.begin(), valid_.end(), 0); }

private:
    int find(uint32_t line) const {
        uint32_t base = (line & (sets_ - 1)) * ways_;
        for (uint32_t w = 0; w < ways_; w++)
            if (valid_[base + w] && line_[base + w] == line) return int(w);
        return -1;
    }
    void touch(uint32_t line, uint32_t way) {
        uint64_t& bits = plru_[line & (sets_ - 1)];
        bits = plru_touch(bits, ways_, way);
    }

    uint32_t sets_, ways_, line_shift_;
    Victim   victim_;
    bool     touch_write_hits_;
    std::vector<uint32_t> line_;
    std::vector<uint8_t>  valid_;
    std::vector<uint64_t> plru_;
};

// ── One hierarchy: L1-I and L1-D over a unified L2 ───────────────
enum class CacheLevel { L1I, L1D, L2 };

struct CacheEnables { bool l1i, l1d, l2; };

// Where a fetch or load was served: its L1, an L1 fill from L2 or from
// memory, or no cache level at all.
enum class Served { L1, L2, Memory, Uncached };

struct CacheCounts {
    uint64_t read_hits = 0, read_misses = 0, write_hits = 0, write_misses = 0;
};

class CacheHierarchy {
public:
    CacheHierarchy(std::string label, const CacheGeometry& l1i,
                   const CacheGeometry& l1d, const CacheGeometry& l2,
                   uint32_t page_bytes)
        : label_(std::move(label)), geom_{l1i, l1d, l2},
          l1i_(l1i, ModelCache::Victim::InvalidFirst, true),
          l1d_(l1d, ModelCache::Victim::InvalidFirst, true),
          l2_(l2, ModelCache::Victim::TreeOnly, false),
          line_words_(l2.line_bytes / 4), page_lines_(page_bytes / l1i.line_bytes) {}

    Served fetch(uint32_t pa, bool cacheable, bool super, const CacheEnables& en) {
        after_miss_fetch();
        return read(Kind::I, l1i_, counts_[L1I], pa, cacheable && en.l1i, en.l2, super);
    }

    Served load(uint32_t pa, bool cacheable, bool super, const CacheEnables& en) {
        after_miss_access(l1d_.line_of(pa));
        return read(Kind::D, l1d_, counts_[L1D], pa, cacheable && en.l1d, en.l2, super);
    }

    void store(uint32_t pa, bool cacheable, bool super, const CacheEnables& en) {
        uint32_t line = l1d_.line_of(pa);
        after_miss_access(line);
        if (cacheable && en.l1d) {
            if (l1d_.write(line)) counts_[L1D].write_hits++;
            else                  counts_[L1D].write_misses++;
        } else {
            uncached_[super][int(Kind::D)][1]++;
        }
        if (cacheable && en.l2) {
            if (l2_.write(line)) counts_[L2].write_hits++;
            else                 counts_[L2].write_misses++;
        }
    }

    void invalidate(CacheLevel level) {
        (level == CacheLevel::L1I ? l1i_ : level == CacheLevel::L1D ? l1d_ : l2_)
            .invalidate_all();
        if (level == CacheLevel::L1I) {
            if (continue_.valid) predicted_.unused++;
            once_.valid = continue_.valid = false;
        }
    }

    void clear_counts() {
        for (CacheCounts& c : counts_) c = CacheCounts();
        for (auto& m : fills_) for (auto& k : m) k[0] = k[1] = 0;
        for (auto& m : uncached_) for (auto& k : m) k[0] = k[1] = 0;
        i_misses_ = i_next_line_ = memory_fills_ = memory_next_line_ = 0;
        predicted_ = PredictionCounts();
        after_miss_counts_ = AfterMissCounts();
    }

    void report(FILE* out, uint64_t insns) const {
        auto kib = [](const CacheGeometry& g) { return g.bytes() / 1024; };
        fprintf(out, "-- cache %s: L1I %u KiB %u-way, L1D %u KiB %u-way, L2 %u KiB %u-way, %u B lines\n",
                label_.c_str(), kib(geom_[L1I]), geom_[L1I].ways, kib(geom_[L1D]),
                geom_[L1D].ways, kib(geom_[L2]), geom_[L2].ways, geom_[L2].line_bytes);
        fprintf(out, "         read hits  read misses   write hits write misses  hit rate\n");
        static const char* const names[] = {"L1I", "L1D", "L2"};
        for (int l = 0; l < 3; l++) {
            const CacheCounts& c = counts_[l];
            uint64_t hits = c.read_hits + c.write_hits;
            uint64_t all  = hits + c.read_misses + c.write_misses;
            if (l == L1I)
                fprintf(out, "  %-4s %12llu %12llu %12s %12s", names[l],
                        (unsigned long long)c.read_hits, (unsigned long long)c.read_misses, "-", "-");
            else
                fprintf(out, "  %-4s %12llu %12llu %12llu %12llu", names[l],
                        (unsigned long long)c.read_hits, (unsigned long long)c.read_misses,
                        (unsigned long long)c.write_hits, (unsigned long long)c.write_misses);
            fprintf(out, "  %6.1f %%\n", all ? 100.0 * hits / all : 0.0);
        }
        double k = insns / 1000.0;
        fprintf(out, "  L1 fills      from L2  from memory   per 1000 instructions\n");
        static const char* const kinds[] = {"I", "D"};
        static const char* const modes[] = {"user", "super"};
        for (int kind = 0; kind < 2; kind++)
            for (int super = 1; super >= 0; super--) {
                const uint64_t* f = fills_[super][kind];
                fprintf(out, "  %s %-5s %12llu %12llu   %8.2f %8.2f\n", kinds[kind], modes[super],
                        (unsigned long long)f[0], (unsigned long long)f[1],
                        k ? f[0] / k : 0.0, k ? f[1] / k : 0.0);
            }
        fprintf(out, "  uncached      fetches %llu, loads %llu, stores %llu\n",
                (unsigned long long)(uncached_[0][0][0] + uncached_[1][0][0]),
                (unsigned long long)(uncached_[0][1][0] + uncached_[1][1][0]),
                (unsigned long long)(uncached_[0][1][1] + uncached_[1][1][1]));
        fprintf(out, "  sequential    %.1f %% of L1-I misses take the line after the previous one's;"
                     " %.1f %% of memory fills continue one of the last two\n",
                i_misses_ ? 100.0 * i_next_line_ / i_misses_ : 0.0,
                memory_fills_ ? 100.0 * memory_next_line_ / memory_fills_ : 0.0);
        const PredictionCounts& p = predicted_;
        fprintf(out, "  next-line     buffer serves %llu (once) and %llu (continue) L1-I misses;"
                     " continue looks up %llu, %llu missing L2, %llu unused\n",
                (unsigned long long)p.once_served, (unsigned long long)p.continue_served,
                (unsigned long long)p.lookups, (unsigned long long)p.missing_l2,
                (unsigned long long)p.unused);
        fprintf(out, "                per 1000 instructions: %.2f, %.2f; %.2f, %.2f, %.2f\n",
                k ? p.once_served / k : 0.0, k ? p.continue_served / k : 0.0,
                k ? p.lookups / k : 0.0, k ? p.missing_l2 / k : 0.0, k ? p.unused / k : 0.0);
        const AfterMissCounts& b = after_miss_counts_;
        fprintf(out, "  after miss    %llu L1-D load misses; the instruction after accesses the"
                     " filled set %llu, another set %llu, no memory %llu\n",
                (unsigned long long)b.misses, (unsigned long long)b.same_set,
                (unsigned long long)b.other_set, (unsigned long long)b.no_access);
        fprintf(out, "                per 1000 instructions: %.2f; %.2f, %.2f, %.2f\n",
                k ? b.misses / k : 0.0, k ? b.same_set / k : 0.0, k ? b.other_set / k : 0.0,
                k ? b.no_access / k : 0.0);
    }

private:
    enum { L1I = 0, L1D = 1, L2 = 2 };
    enum class Kind { I = 0, D = 1 };

    Served read(Kind kind, ModelCache& l1, CacheCounts& c, uint32_t pa, bool l1_active,
                bool l2_enabled, bool super) {
        if (!l1_active) {           // forwarded uncacheable: no level keeps it
            uncached_[super][int(kind)][0]++;
            return Served::Uncached;
        }
        uint32_t line = l1.line_of(pa);
        if (l1.read(line)) { c.read_hits++; return Served::L1; }
        c.read_misses++;
        if (kind == Kind::D) record_load_miss(line);
        if (kind == Kind::I) {
            i_misses_++;
            if (line == last_i_miss_ + 1) i_next_line_++;
            last_i_miss_ = line;
        }
        bool from_l2 = false;
        if (l2_enabled) {
            if (l2_.read(line)) {
                from_l2 = true;
                counts_[L2].read_hits += line_words_;
            } else {
                counts_[L2].read_misses++;
                counts_[L2].read_hits += line_words_ - 1;
                l2_.fill(line);
            }
        }
        if (!from_l2) {
            memory_fills_++;
            if (line == stream_[0] + 1)      { memory_next_line_++; stream_[0] = line; }
            else if (line == stream_[1] + 1) { memory_next_line_++; stream_[1] = line; }
            else                             { stream_[1] = stream_[0]; stream_[0] = line; }
        }
        fills_[super][int(kind)][from_l2 ? 0 : 1]++;
        l1.fill(line);
        if (kind == Kind::I) next_line_miss(line, l2_enabled);
        return from_l2 ? Served::L2 : Served::Memory;
    }

    // The instruction after an L1-D load miss.  The ISS fetches every
    // instruction before it accesses memory, so instructions are numbered
    // by their fetches: the instruction after the miss is miss_insn_ + 1,
    // and a fetch numbered miss_insn_ + 2 means it made no access.
    void record_load_miss(uint32_t line) {
        miss_insn_ = insn_;
        miss_set_ = line & (geom_[L1D].sets - 1);
        miss_pending_ = true;
        after_miss_counts_.misses++;
    }

    void after_miss_fetch() {
        insn_++;
        if (miss_pending_ && insn_ == miss_insn_ + 2) {
            after_miss_counts_.no_access++;
            miss_pending_ = false;
        }
    }

    void after_miss_access(uint32_t line) {
        if (miss_pending_ && insn_ == miss_insn_ + 1) {
            if (miss_set_ == (line & (geom_[L1D].sets - 1)))
                after_miss_counts_.same_set++;
            else
                after_miss_counts_.other_set++;
            miss_pending_ = false;
        }
    }

    // The prediction buffers at an L1-I miss to `line`: count whether each
    // held it, then look up the line after it as the policy says.
    void next_line_miss(uint32_t line, bool l2_enabled) {
        bool once_hit = once_.valid && once_.line == line;
        bool continue_hit = continue_.valid && continue_.line == line;
        if (once_hit) predicted_.once_served++;
        if (continue_hit) predicted_.continue_served++;
        else if (continue_.valid) predicted_.unused++;
        once_.valid = continue_.valid = false;
        if (!l2_enabled) return;

        Prediction next = predict(line + 1);
        if (next != Prediction::OtherPage) predicted_.lookups++;
        if (next == Prediction::MissesL2) predicted_.missing_l2++;
        continue_ = { next == Prediction::Loaded, line + 1 };
        if (!once_hit) once_ = continue_;
    }

    // Whether the buffer can hold `next_line`, the line after an L1-I miss:
    // the buffer is filled from L2 only, never across a page.
    enum class Prediction { OtherPage, MissesL2, Loaded };
    Prediction predict(uint32_t next_line) const {
        if ((next_line & (page_lines_ - 1)) == 0)
            return Prediction::OtherPage;

        if (!l2_.contains(next_line))
            return Prediction::MissesL2;

        return Prediction::Loaded;
    }

    std::string   label_;
    CacheGeometry geom_[3];
    ModelCache    l1i_, l1d_, l2_;
    uint32_t      line_words_;

    CacheCounts counts_[3];
    uint64_t    fills_[2][2][2] = {};     // [super][kind][from memory]
    uint64_t    uncached_[2][2][2] = {};  // [super][kind][store]
    uint64_t    i_misses_ = 0, i_next_line_ = 0, memory_fills_ = 0, memory_next_line_ = 0;
    uint32_t    last_i_miss_ = 0, stream_[2] = {0, 0};

    struct NextLine { bool valid = false; uint32_t line = 0; };
    // A prediction counts as unused when it is replaced or invalidated
    // without serving a miss.
    struct PredictionCounts {
        uint64_t once_served = 0, continue_served = 0;
        uint64_t lookups = 0, missing_l2 = 0, unused = 0;   // continue's
    };
    uint32_t         page_lines_;
    NextLine         once_, continue_;
    PredictionCounts predicted_;

    struct AfterMissCounts { uint64_t misses = 0, same_set = 0, other_set = 0, no_access = 0; };
    uint64_t        insn_ = 0;             // the current instruction, numbered by its fetch
    uint64_t        miss_insn_ = 0;        // the last load to miss L1-D
    uint32_t        miss_set_ = 0;         // the L1-D set its line goes into
    bool            miss_pending_ = false; // the instruction after it not yet counted
    AfterMissCounts after_miss_counts_;
};

// ── The model: every hierarchy +cachemodel names ─────────────────
class CacheModel {
public:
    // Parse the +cachemodel value: empty for the presented generation, or
    // a comma-separated list of generation names and explicit hierarchies
    // "L1I:L1D:L2", each level SIZE/WAYS (SIZE in bytes, or with a k or m
    // suffix).  Lines are 16 bytes, as every generation's are; pages are
    // the MMU's.
    static bool parse(const char* spec, const CoreGeneration& presented,
                      uint32_t page_bytes, CacheModel& model, std::string& err) {
        model.page_bytes_ = page_bytes;
        std::string s(spec);
        if (s.empty()) {
            model.add(presented.name, presented.l1i, presented.l1d, presented.l2);
            return true;
        }
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!add_item(item, model, err)) return false;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return true;
    }

    // Each returns what served the access in the first hierarchy listed.
    Served fetch(uint32_t pa, bool c, bool super, const CacheEnables& en) {
        Served first = h_[0].fetch(pa, c, super, en);
        for (size_t i = 1; i < h_.size(); i++) h_[i].fetch(pa, c, super, en);
        return first;
    }
    Served load(uint32_t pa, bool c, bool super, const CacheEnables& en) {
        Served first = h_[0].load(pa, c, super, en);
        for (size_t i = 1; i < h_.size(); i++) h_[i].load(pa, c, super, en);
        return first;
    }
    void store(uint32_t pa, bool c, bool super, const CacheEnables& en) { for (auto& h : h_) h.store(pa, c, super, en); }
    void invalidate(CacheLevel level) { for (auto& h : h_) h.invalidate(level); }
    void clear_counts() { for (auto& h : h_) h.clear_counts(); }
    void report(FILE* out, uint64_t insns) const { for (const auto& h : h_) h.report(out, insns); }

private:
    void add(std::string label, const CacheGeometry& l1i, const CacheGeometry& l1d,
             const CacheGeometry& l2) {
        h_.emplace_back(std::move(label), l1i, l1d, l2, page_bytes_);
    }

    static bool add_item(const std::string& item, CacheModel& model, std::string& err) {
        if (const CoreGeneration* g = find_core_generation(item.c_str())) {
            model.add(item, g->l1i, g->l1d, g->l2);
            return true;
        }
        CacheGeometry level[3];
        size_t pos = 0;
        for (int i = 0; i < 3; i++) {
            size_t colon = item.find(':', pos);
            if ((i < 2) != (colon != std::string::npos)) {
                err = "'" + item + "' is neither a generation nor L1I:L1D:L2";
                return false;
            }
            std::string field = item.substr(pos, i < 2 ? colon - pos : std::string::npos);
            if (!parse_level(field, i == 2 ? CACHE_ADDRESSING_PIPT : CACHE_ADDRESSING_VIPT,
                             level[i], err)) {
                err = "'" + item + "': " + err;
                return false;
            }
            pos = colon + 1;
        }
        model.add(item, level[0], level[1], level[2]);
        return true;
    }

    // SIZE/WAYS, with 16-byte lines: a power-of-two way count up to 64 and
    // a power-of-two number of sets.
    static bool parse_level(const std::string& field, uint32_t addressing,
                            CacheGeometry& g, std::string& err) {
        const uint32_t line = 16;
        char* end = nullptr;
        unsigned long size = strtoul(field.c_str(), &end, 10);
        if (*end == 'k' || *end == 'K') { size *= 1024; end++; }
        else if (*end == 'm' || *end == 'M') { size *= 1024 * 1024; end++; }
        if (*end != '/') { err = "level '" + field + "' is not SIZE/WAYS"; return false; }
        char* rest = nullptr;
        unsigned long ways = strtoul(end + 1, &rest, 10);
        auto pow2 = [](unsigned long v) { return v && !(v & (v - 1)); };
        if (*rest || !pow2(ways) || ways > 64) {
            err = "level '" + field + "': ways must be a power of two up to 64";
            return false;
        }
        if (size % (ways * line) || !pow2(size / (ways * line))) {
            err = "level '" + field + "': size must be a power-of-two number of " +
                  std::to_string(ways) + "-way sets of 16-byte lines";
            return false;
        }
        g = { uint32_t(size / (ways * line)), uint32_t(ways), line, addressing };
        return true;
    }

    uint32_t page_bytes_ = 0;
    std::vector<CacheHierarchy> h_;
};

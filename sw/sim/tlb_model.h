// tlb_model.h — shadow TLBs for the ISS (+tlbmodel).
//
// The main TLB is software-managed: a miss traps, and the kernel's
// handler walks the page table and installs the entry.  A TLB model
// watches the translations the main TLB serves — every successful one,
// by VPN, ASID and the entry's G bit; the pinned slots never reach it —
// and replays them through one or more shadow TLBs of other geometries,
// counting what each would have missed.  The refills the running
// software actually took over the same window are counted beside them,
// so a shadow of the built geometry calibrates the rest.
//
// A shadow fills itself on a miss; no handler runs.  Its victim is
// random by default — what a refill that picks the way from a
// free-running counter gives, ignoring whether a way is empty — or,
// with "lru", the first invalid way, else the least recently used.  Its
// set is the VPN's low bits, or with "hash" the VPN folded by XOR in
// index-width chunks.
//
// A shadow can only see what software does to the real TLB.  An entry
// the real TLB lost to a refill stays in a larger shadow, which is the
// point of one; an entry software invalidates in the real TLB is
// dropped from the shadows too.  One the software drops from its page
// tables while the real TLB no longer holds it has nothing to invalidate
// there, so a larger shadow keeps it: shadows larger than the built TLB
// read slightly optimistic.  Invalidations reach the shadows by the
// translation the slot held, not by slot, which follows the MMU only
// while the main TLB holds one entry per translation — nothing the MMU
// enforces, but nothing software has a use for either.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "penumbra_cores.h"

// ── One shadow TLB ───────────────────────────────────────────────
class ShadowTlb {
public:
    enum class Victim { Random, Lru };

    ShadowTlb(std::string label, TlbGeometry geometry, Victim victim, bool hashed)
        : label_(std::move(label)), geom_(geometry), victim_(victim),
          hashed_(hashed && geometry.sets > 1),
          index_bits_(__builtin_ctz(geometry.sets)),
          entries_(geometry.sets * geometry.ways) {}

    void translate(uint32_t vpn, uint8_t asid, bool global, bool super) {
        Entry* set = &entries_[set_of(vpn) * geom_.ways];
        use_clock_++;
        for (uint32_t w = 0; w < geom_.ways; w++)
            if (set[w].valid && set[w].vpn == vpn && (set[w].global || set[w].asid == asid)) {
                set[w].used = use_clock_;
                return;
            }
        misses_[super]++;
        set[pick_victim(set)] = { true, global, asid, vpn, use_clock_ };
    }

    // Drop the shadow's copy of a real entry.  A global copy is found
    // whatever ASID it recorded: it filled under whichever ASID was
    // current, while software may install global entries under any.
    void invalidate(uint32_t vpn, uint8_t asid, bool global) {
        Entry* set = &entries_[set_of(vpn) * geom_.ways];
        for (uint32_t w = 0; w < geom_.ways; w++)
            if (set[w].valid && set[w].vpn == vpn && set[w].global == global &&
                (global || set[w].asid == asid))
                set[w].valid = false;
    }

    void clear_counts() { misses_[0] = misses_[1] = 0; }

    std::string layout() const {
        return std::to_string(geom_.sets) + "x" + std::to_string(geom_.ways) +
               (victim_ == Victim::Lru ? "/lru" : "/rnd") + (hashed_ ? "/hash" : "");
    }

    const std::string& label() const { return label_; }
    uint64_t misses(bool super) const { return misses_[super]; }

private:
    struct Entry {
        bool     valid = false, global = false;
        uint8_t  asid = 0;
        uint32_t vpn = 0;
        uint64_t used = 0;
    };

    uint32_t set_of(uint32_t vpn) const {
        if (!hashed_) return vpn & (geom_.sets - 1);
        uint32_t folded = 0;
        for (uint32_t v = vpn; v; v >>= index_bits_) folded ^= v;
        return folded & (geom_.sets - 1);
    }

    uint32_t pick_victim(const Entry* set) {
        if (victim_ == Victim::Random) {
            // xorshift32: a fixed seed keeps runs repeatable.
            rng_ ^= rng_ << 13;
            rng_ ^= rng_ >> 17;
            rng_ ^= rng_ << 5;
            return rng_ % geom_.ways;
        }
        uint32_t victim = 0;
        for (uint32_t w = 0; w < geom_.ways; w++) {
            if (!set[w].valid) return w;
            if (set[w].used < set[victim].used) victim = w;
        }
        return victim;
    }

    std::string        label_;
    TlbGeometry        geom_;
    Victim             victim_;
    bool               hashed_;
    uint32_t           index_bits_;
    std::vector<Entry> entries_;
    uint64_t           use_clock_ = 0;
    uint32_t           rng_ = 0xACE1u;
    uint64_t           misses_[2] = {};     // [super]
};

// ── The model: every shadow +tlbmodel names ──────────────────────
class TlbModel {
public:
    // Parse the +tlbmodel value: empty for the presented generation's TLB,
    // or a comma-separated list of generation names and explicit layouts
    // SETSxWAYS, each optionally followed by /lru or /rnd and /hash.  A
    // generation stands for its built geometry with random replacement.
    static bool parse(const char* spec, const CoreGeneration& presented,
                      TlbModel& model, std::string& err) {
        std::string s(spec);
        if (s.empty()) {
            model.add(presented.name, presented.tlb, ShadowTlb::Victim::Random, false);
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

    // A main-TLB translation that succeeded, pinned slots excluded.
    void translate(uint32_t vpn, uint8_t asid, bool global, bool super) {
        translations_++;
        for (auto& t : t_) t.translate(vpn, asid, global, super);
    }

    // A main-TLB miss the running software has to refill.
    void real_miss(bool super) { real_misses_[super]++; }

    // Software wrote a main-TLB slot: `old_*` is what the slot held
    // (VPN word {VPN, ASID}, as vpn_of() and asid_of() split it; PTE
    // word with the TLB_V and TLB_G flags in the low byte), `new_pte`
    // what replaces it.  The writes that invalidate an entry are the
    // ones the shadows mirror, through invalidate(); a refill replacing
    // a victim is not one of them.  The old slot names the entry: an
    // invalidating write commits whatever VPN word was last staged.
    void real_slot_written(uint32_t old_vpn_word, uint32_t old_pte, uint32_t new_pte) {
        uint32_t vpn = vpn_of(old_vpn_word);
        uint32_t asid = asid_of(old_vpn_word);
        bool g = old_pte & TLB_G;
        if (!(new_pte & TLB_V))
            invalidate(vpn, asid, g);
    }

    void clear_counts() {
        translations_ = real_misses_[0] = real_misses_[1] = 0;
        for (auto& t : t_) t.clear_counts();
    }

    void report(FILE* out, uint64_t insns, const TlbGeometry& built) const {
        double k = insns / 1000.0;
        fprintf(out, "-- tlb: %llu main-TLB translations\n", (unsigned long long)translations_);
        fprintf(out, "  %-20s %-14s %12s %12s %12s  per 1000 instructions\n",
                "shadow", "layout", "misses", "user", "super");
        auto row = [&](const std::string& label, const std::string& layout,
                       uint64_t user, uint64_t super) {
            fprintf(out, "  %-20s %-14s %12llu %12llu %12llu  %8.2f\n", label.c_str(),
                    layout.c_str(), (unsigned long long)(user + super),
                    (unsigned long long)user, (unsigned long long)super,
                    k ? (user + super) / k : 0.0);
        };
        row("real", std::to_string(built.sets) + "x" + std::to_string(built.ways) + "/software",
            real_misses_[0], real_misses_[1]);
        for (const auto& t : t_) row(t.label(), t.layout(), t.misses(false), t.misses(true));
    }

private:
    static constexpr uint32_t TLB_V = 1u << 0, TLB_G = 1u << 7;
    static uint32_t vpn_of(uint32_t vpn_word)  { return (vpn_word >> 8) & 0xFFFFF; }
    static uint8_t  asid_of(uint32_t vpn_word) { return vpn_word & 0xFF; }

    void invalidate(uint32_t vpn, uint8_t asid, bool global) {
        for (auto& t : t_) t.invalidate(vpn, asid, global);
    }

    void add(std::string label, TlbGeometry g, ShadowTlb::Victim victim, bool hashed) {
        t_.emplace_back(std::move(label), g, victim, hashed);
    }

    // SETSxWAYS[/lru|/rnd][/hash]: a power-of-two number of sets up to
    // 4096 and 1 to 64 ways.
    static bool add_item(const std::string& item, TlbModel& model, std::string& err) {
        if (const CoreGeneration* g = find_core_generation(item.c_str())) {
            model.add(item, g->tlb, ShadowTlb::Victim::Random, false);
            return true;
        }
        char* end = nullptr;
        unsigned long sets = strtoul(item.c_str(), &end, 10);
        if (*end != 'x') {
            err = "'" + item + "' is neither a generation nor SETSxWAYS";
            return false;
        }
        unsigned long ways = strtoul(end + 1, &end, 10);
        if (!sets || (sets & (sets - 1)) || sets > 4096 || !ways || ways > 64) {
            err = "'" + item + "': sets must be a power of two up to 4096, ways 1 to 64";
            return false;
        }
        ShadowTlb::Victim victim = ShadowTlb::Victim::Random;
        bool hashed = false;
        for (std::string rest(end); !rest.empty();) {
            size_t next = rest.find('/', 1);
            std::string opt = rest.substr(0, next);
            if (opt == "/lru")       victim = ShadowTlb::Victim::Lru;
            else if (opt == "/rnd")  victim = ShadowTlb::Victim::Random;
            else if (opt == "/hash") hashed = true;
            else {
                err = "'" + item + "': unknown option '" + opt + "' (lru, rnd, hash)";
                return false;
            }
            rest = next == std::string::npos ? "" : rest.substr(next);
        }
        model.add(item, { uint32_t(sets), uint32_t(ways) }, victim, hashed);
        return true;
    }

    std::vector<ShadowTlb> t_;
    uint64_t translations_ = 0;
    uint64_t real_misses_[2] = {};     // [super]
};

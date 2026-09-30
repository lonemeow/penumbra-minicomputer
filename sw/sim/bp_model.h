// bp_model.h — shadow branch predictors (+bpmodel).
//
// Every control transfer the ISS executes — its address, kind, outcome
// and target — runs through one or more predictors side by side, each
// counting where the front end would have redirected fetch to the right
// path: at fetch from a branch target buffer, at the IF2 pre-decode
// from the instruction itself, or only when the transfer resolves in EX.
// The counts are exact for the architectural stream; the cycles they
// are priced at are estimates.
//
// A predictor combines any of:
//
//  - btbN / ibtbN: a direct-mapped branch target buffer of N entries,
//    each tagged with the rest of the address.  btb holds direct
//    transfers only (B, Bcc, BL), as gen2.5's does; ibtb also holds
//    indirect ones (JMP, JALR), returns included.  A transfer that
//    resolves taken writes its entry.  Without a direction table, a
//    conditional branch the buffer holds predicts taken and one that
//    resolves not taken removes its entry, so each site predicts the
//    direction it last went.
//  - bimN / gsN: a direction table of N two-bit counters for conditional
//    branches, indexed by the address (bimodal) or by the address XORed
//    with the outcomes of the last log2(N) conditional branches (gshare).
//  - rasN: a return-address stack of depth N at the IF2 pre-decode,
//    a plain ring.  Calls (BL, JALR) push their return address; a return
//    (JMP R13, the ABI's RET) predicts the top slot, overriding the
//    buffer, and steps back whether or not that slot still holds a
//    return.  rasNe also counts its entries and predicts nothing once
//    they run out.
//  - dec: the IF2 pre-decode redirects a direct transfer the buffer did
//    not predict: B and BL always, Bcc when the direction table says
//    taken, or without one when it branches backward.
//
// Tables train with each transfer's outcome as it executes; hardware
// trains a few cycles later, and its global history is speculative, so
// the model reads slightly optimistic on tight loops.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Cycles a redirect costs beyond the transfer's own issue cycle.  A
// buffer read beside IF1 answers in IF2, as the pre-decode does: one
// discarded fetch either way, as gen2.5's predicted taken branch costs.
// A transfer resolved in EX costs what gen2.5's unpredicted one does
// (doc/system/instruction-timing.md, "Control transfers").
static constexpr double BP_FETCH_REDIRECT  = 1;
static constexpr double BP_DECODE_REDIRECT = 1;
static constexpr double BP_EXECUTE_REDIRECT = 4;

// Bcc; B; BL; JALR; JMP R13; any other JMP.
enum class BranchKind { Conditional, Direct, Call, IndirectCall, Return, Indirect };
static constexpr int BRANCH_KINDS = 6;

// ── Return-address stack ─────────────────────────────────────────
class ReturnStack {
public:
    ReturnStack(uint32_t depth, bool checks_empty)
        : slots_(depth), checks_empty_(checks_empty) {}

    // A call's return address.  A ring: a push that finds every slot
    // taken overwrites the oldest, since the innermost frames are the
    // returns worth predicting, and a stack that call/return tricks
    // (longjmp, a return by another route) have desynced keeps
    // predicting rather than filling up and refusing.
    void push(uint32_t ret) {
        top_ = (top_ + 1) % slots_.size();
        slots_[top_] = ret;
        count_ = std::min(count_ + 1, (uint32_t)slots_.size());
    }

    // The predicted return address — the most recent push — or false
    // when the stack counts its entries and has none.  A plain ring
    // predicts whatever its top slot holds: a return that finds it stale
    // costs the same redirect as one predicted nothing.
    bool pop(uint32_t& ret) {
        if (checks_empty_ && !count_) {
            ret = 0;
            return false;
        }
        ret = slots_[top_];
        top_ = (top_ + slots_.size() - 1) % slots_.size();
        if (count_) count_--;
        return true;
    }

private:
    std::vector<uint32_t> slots_;
    bool     checks_empty_;
    uint32_t top_ = 0;      // index of the most recent push
    uint32_t count_ = 0;    // valid entries, at most slots_.size()
};

// ── Direction table: two-bit counters ────────────────────────────
class DirectionTable {
public:
    DirectionTable(uint32_t entries, bool gshare)
        : counters_(entries, 1), mask_(entries - 1), gshare_(gshare) {}

    bool predict(uint32_t pc) const { return counters_[index(pc)] >= 2; }

    void update(uint32_t pc, bool taken) {
        uint8_t& c = counters_[index(pc)];
        if (taken && c < 3) c++;
        if (!taken && c > 0) c--;
        if (gshare_) history_ = ((history_ << 1) | taken) & mask_;
    }

private:
    uint32_t index(uint32_t pc) const {
        uint32_t i = pc >> 2;
        if (gshare_) i ^= history_;
        return i & mask_;
    }

    std::vector<uint8_t> counters_;   // 0..1 not taken, 2..3 taken; start weakly not taken
    uint32_t mask_;
    bool     gshare_;
    uint32_t history_ = 0;
};

// ── Branch target buffer ─────────────────────────────────────────
class TargetBuffer {
public:
    TargetBuffer(uint32_t entries, bool indirect)
        : entries_(entries), index_bits_(__builtin_ctz(entries)), indirect_(indirect) {}

    bool holds(BranchKind k) const {
        return indirect_ || k == BranchKind::Conditional || k == BranchKind::Direct
                         || k == BranchKind::Call;
    }

    bool lookup(uint32_t pc, uint32_t& target) const {
        const Entry& e = entries_[slot(pc)];
        if (!e.valid || e.tag != tag(pc)) return false;
        target = e.target;
        return true;
    }

    void write(uint32_t pc, uint32_t target) { entries_[slot(pc)] = { true, tag(pc), target }; }

    void remove(uint32_t pc) {
        Entry& e = entries_[slot(pc)];
        if (e.valid && e.tag == tag(pc)) e.valid = false;
    }

private:
    struct Entry { bool valid = false; uint32_t tag = 0, target = 0; };
    uint32_t slot(uint32_t pc) const { return (pc >> 2) & (entries_.size() - 1); }
    uint32_t tag(uint32_t pc) const { return (pc >> 2) >> index_bits_; }

    std::vector<Entry> entries_;
    uint32_t index_bits_;
    bool     indirect_;
};

// ── One predictor ────────────────────────────────────────────────
struct BpConfig {
    std::string label;
    uint32_t    btb = 0;        // entries, 0 for none
    bool        ibtb = false;   // the buffer holds indirect transfers too
    uint32_t    dir = 0;        // direction-table entries, 0 for none
    bool        gshare = false;
    uint32_t    ras = 0;        // depth, 0 for none
    bool        ras_checks_empty = false;
    bool        decode = false; // IF2 pre-decode redirects direct transfers
};

class BranchPredictor {
public:
    explicit BranchPredictor(BpConfig c) : cfg_(std::move(c)) {
        if (cfg_.btb) btb_.emplace_back(cfg_.btb, cfg_.ibtb);
        if (cfg_.dir) dir_.emplace_back(cfg_.dir, cfg_.gshare);
        if (cfg_.ras) ras_.emplace_back(cfg_.ras, cfg_.ras_checks_empty);
    }

    // One executed control transfer: `pc` its address, `target` where it
    // goes when taken.  Calls and returns are always taken.
    void transfer(uint32_t pc, BranchKind kind, bool taken, uint32_t target) {
        int k = int(kind);
        counts_.transfers[k]++;
        if (taken) counts_.taken[k]++;

        // Fetch: the buffer, with the direction table's say for a
        // conditional branch.
        enum class Where { None, Fetch, Decode } where = Where::None;
        uint32_t predicted = 0;
        uint32_t buffered = 0;
        bool in_btb = !btb_.empty() && btb_[0].holds(kind) && btb_[0].lookup(pc, buffered);
        if (in_btb) {
            bool go = kind != BranchKind::Conditional || dir_.empty() || dir_[0].predict(pc);
            if (go) { where = Where::Fetch; predicted = buffered; }
        }

        // Decode: a return takes the stack's top; a direct transfer the
        // buffer did not send elsewhere is redirected from its own word.
        if (kind == BranchKind::Return && !ras_.empty()) {
            uint32_t top;
            if (ras_[0].pop(top)) {
                if (!(where == Where::Fetch && predicted == top)) {
                    where = Where::Decode;
                    predicted = top;
                }
            }
        } else if (cfg_.decode && where == Where::None && is_direct(kind)) {
            bool go = kind != BranchKind::Conditional
                   || (dir_.empty() ? target < pc : dir_[0].predict(pc));
            if (go) { where = Where::Decode; predicted = target; }
        }
        bool links = kind == BranchKind::Call || kind == BranchKind::IndirectCall;
        if (links && !ras_.empty()) ras_[0].push(pc + 4);

        // Resolve.
        bool predicted_taken = where != Where::None;
        if (taken && predicted_taken && predicted == target) {
            if (where == Where::Fetch) counts_.fetch++;
            else                       counts_.decode++;
        } else if (taken || predicted_taken) {
            counts_.execute[k]++;
        }

        // Train.
        if (kind == BranchKind::Conditional && !dir_.empty()) dir_[0].update(pc, taken);
        if (!btb_.empty() && btb_[0].holds(kind)) {
            if (taken)              btb_[0].write(pc, target);
            else if (dir_.empty())  btb_[0].remove(pc);
        }
    }

    void clear_counts() { counts_ = Counts(); }

    void report(FILE* out, uint64_t insns) const {
        const Counts& c = counts_;
        fprintf(out, "-- branch predictor %s\n", cfg_.label.c_str());
        fprintf(out, "  transfers (taken): conditional %llu (%llu), direct %llu, calls %llu,"
                     " indirect calls %llu, returns %llu, indirect %llu\n",
                (unsigned long long)c.transfers[0], (unsigned long long)c.taken[0],
                (unsigned long long)c.transfers[1], (unsigned long long)c.transfers[2],
                (unsigned long long)c.transfers[3], (unsigned long long)c.transfers[4],
                (unsigned long long)c.transfers[5]);
        uint64_t ex = 0;
        for (int k = 0; k < BRANCH_KINDS; k++) ex += c.execute[k];
        fprintf(out, "  redirects: fetch %llu, decode %llu, execute %llu (conditional %llu,"
                     " direct %llu, calls %llu, indirect calls %llu, returns %llu, indirect %llu)\n",
                (unsigned long long)c.fetch, (unsigned long long)c.decode, (unsigned long long)ex,
                (unsigned long long)c.execute[0], (unsigned long long)c.execute[1],
                (unsigned long long)c.execute[2], (unsigned long long)c.execute[3],
                (unsigned long long)c.execute[4], (unsigned long long)c.execute[5]);
        double k = insns / 1000.0;
        double fetch = c.fetch * BP_FETCH_REDIRECT, decode = c.decode * BP_DECODE_REDIRECT,
               execute = ex * BP_EXECUTE_REDIRECT;
        fprintf(out, "  cycles per 1000 instructions: %.1f (fetch %.1f, decode %.1f, execute %.1f;"
                     " returns %.1f)\n",
                k ? (fetch + decode + execute) / k : 0.0, k ? fetch / k : 0.0, k ? decode / k : 0.0,
                k ? execute / k : 0.0,
                k ? c.execute[int(BranchKind::Return)] * BP_EXECUTE_REDIRECT / k : 0.0);
    }

private:
    static bool is_direct(BranchKind k) {
        return k == BranchKind::Conditional || k == BranchKind::Direct || k == BranchKind::Call;
    }

    struct Counts {
        uint64_t transfers[BRANCH_KINDS] = {}, taken[BRANCH_KINDS] = {};
        uint64_t fetch = 0, decode = 0, execute[BRANCH_KINDS] = {};
    };

    BpConfig                    cfg_;
    std::vector<TargetBuffer>   btb_;
    std::vector<DirectionTable> dir_;
    std::vector<ReturnStack>    ras_;
    Counts                      counts_;
};

// ── The model: every predictor +bpmodel names ────────────────────
class BpModel {
public:
    // Parse the +bpmodel value: empty for a default comparison, or a
    // comma-separated list of predictors, each "none" or components
    // joined by '+': btbN, ibtbN, bimN, gsN, rasN, rasNe, dec.
    static bool parse(const char* spec, BpModel& model, std::string& err) {
        std::string s(*spec ? spec
            : "none,btb32,dec,dec+ras8,ibtb32+ras8+dec,dec+bim1k+ras8,"
              "ibtb256+bim1k+ras8+dec,ibtb256+gs1k+ras8+dec,ibtb256+gs4k+ras8+dec");
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            BpConfig c;
            if (!parse_item(item, c, err)) {
                err = "'" + item + "': " + err;
                return false;
            }
            model.predictors_.emplace_back(c);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return true;
    }

    void transfer(uint32_t pc, BranchKind kind, bool taken, uint32_t target) {
        for (auto& p : predictors_) p.transfer(pc, kind, taken, target);
    }
    void clear_counts() { for (auto& p : predictors_) p.clear_counts(); }
    void report(FILE* out, uint64_t insns) const { for (const auto& p : predictors_) p.report(out, insns); }

private:
    // A power-of-two size with an optional k suffix.
    static bool parse_size(const std::string& s, uint32_t& n) {
        char* end = nullptr;
        unsigned long v = strtoul(s.c_str(), &end, 10);
        if (*end == 'k' || *end == 'K') { v *= 1024; end++; }
        if (end == s.c_str() || *end || !v || (v & (v - 1)) || v > (1u << 20)) return false;
        n = uint32_t(v);
        return true;
    }

    static bool parse_item(const std::string& item, BpConfig& c, std::string& err) {
        c.label = item;
        if (item == "none") return true;
        size_t pos = 0;
        while (pos <= item.size()) {
            size_t plus = item.find('+', pos);
            std::string part = item.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            auto sized = [&](const char* prefix, uint32_t& n) {
                size_t len = strlen(prefix);
                return part.compare(0, len, prefix) == 0 && parse_size(part.substr(len), n);
            };
            if (part == "dec")                          c.decode = true;
            else if (sized("ibtb", c.btb))              c.ibtb = true;
            else if (sized("btb", c.btb))               c.ibtb = false;
            else if (sized("bim", c.dir))               c.gshare = false;
            else if (sized("gs", c.dir))                c.gshare = true;
            else if (sized("ras", c.ras))               c.ras_checks_empty = false;
            else if (part.size() > 4 && part.back() == 'e'
                     && part.compare(0, 3, "ras") == 0
                     && parse_size(part.substr(3, part.size() - 4), c.ras))
                c.ras_checks_empty = true;
            else {
                err = "unknown component '" + part + "' (btbN, ibtbN, bimN, gsN, rasN, rasNe,"
                      " dec; sizes a power of two)";
                return false;
            }
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
        return true;
    }

    std::vector<BranchPredictor> predictors_;
};

// loaduse_model.h — shadow single-issue pipelines that time the wait
// behind a load (+loadusemodel).
//
// Every instruction the ISS executes runs through one or more models of
// a single-issue pipeline, each counting the cycles instructions wait
// for a register or the flags.  An instruction takes the EX slot after
// the one ahead of it, or a later one when it has to wait:
//
//  - What an ALU computes at EX, and a constant, are usable by the op in
//    the next slot.  A load's value is usable `wait` slots later.
//  - A load or store needs its base at EX, and a jump its target.
//  - The late stage is 1 + wait slots behind EX, where a load's value
//    arrives.  Whatever an op writes is usable by the late stage of the
//    op behind it, so nothing waits there: a store, which needs its data
//    at that stage, never waits for it.
//  - Where the pipeline has its ALU says when an op needs its operands,
//    an ADC, SBC or CMPC its carry and a conditional branch its flags.
//
// An item names one pipeline: `ex` or `late`, then modifiers joined by
// '+':
//
//  - ex: one ALU, at EX.  An op waits for what it computes on to be
//    usable there.
//  - ex+late: a second ALU, at the late stage.  An op whose operands are
//    not usable at EX does not wait: it takes its slot and executes at
//    the late stage, and its result is usable 1 + wait slots later than
//    it would be from EX.  A conditional branch whose flags are not
//    usable at EX resolves at the late stage, and costs 1 + wait cycles
//    more when it was mispredicted.
//  - late: one ALU, at the late stage, where every op that computes
//    executes and every conditional branch resolves: the order of a
//    pipeline that forms an address a stage ahead of its cache and has
//    the ALU behind it.
//  - wait:N: the load's wait (2 unless given).
//
// Mispredictions are those of a pre-decode redirect with a 1K bimodal
// table (dec+bim1k in bp_model.h).
//
// The waits are those behind a register or the flags alone.  Every
// access hits, the next instruction is always there to take a slot, MUL
// and DIV read their operands at EX and take one cycle, and an
// instruction that traps takes no slot.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bp_model.h"
#include "issue_op.h"

// Where a pipeline has its ALU.
enum class AluPlacement { Ex, ExAndLate, Late };

// What an op waits for.  A wait two needs would cause is charged to the
// one that asks for the later slot, and to the first of these when they
// ask for the same.
enum class Need { Operand, Base, Flags, Target };
static constexpr int NEEDS = 4;
static const char* const NEED_NAMES[NEEDS] = { "operand", "base", "flags", "target" };

// ── One pipeline ─────────────────────────────────────────────────
struct LoadUseConfig {
    std::string  label;
    AluPlacement alu = AluPlacement::Ex;
    uint64_t     wait = 2;      // wait:N — slots a load's value is usable later than an ALU result
};

class LoadUsePipeline {
public:
    explicit LoadUsePipeline(LoadUseConfig c) : cfg_(std::move(c)) {}

    // One executed instruction that did not trap, in order.  `mispredicted`
    // says a conditional branch went the way the front end did not expect.
    void insn(const IssueOp& op, bool mispredicted) {
        const uint64_t open = slot_ + 1;    // the first EX slot open to it
        uint64_t slot = open;               // the EX slot it takes
        Need why = Need::Operand;           // the need that asked for that slot, when it is not `open`
        bool late = false;                  // it executes, or resolves, at the late stage
        auto needs = [&](uint64_t from, Need need) {
            if (from > slot) { slot = from; why = need; }
        };

        bool computes = uses_alu(op) && op.produces != Producer::Constant;
        if (computes) {
            uint64_t ready = usable_at(op.reads[int(Use::Operand)]);
            if (op.reads_carry) ready = std::max(ready, flags_usable_);
            late = executes_late(open, ready);
            if (!late) needs(ready, Need::Operand);
        } else if (op.cls == IssueClass::MulDiv) {
            needs(usable_at(op.reads[int(Use::Operand)]), Need::Operand);
        } else if (op.cls == IssueClass::Load || op.cls == IssueClass::Store) {
            needs(usable_at(op.reads[int(Use::Base)]), Need::Base);
        } else if (op.cls == IssueClass::Branch) {
            needs(usable_at(op.reads[int(Use::Target)]), Need::Target);
            if (op.reads_cond) {
                late = executes_late(open, flags_usable_);
                if (!late) needs(flags_usable_, Need::Flags);
            }
        }
        slot_ = slot;

        // When what it writes is usable at EX.
        uint64_t usable = slot + 1;
        if (op.cls == IssueClass::Load) usable += cfg_.wait;
        else if (late)                  usable += late_offset();
        for (int r = 1; r < 15; r++)
            if (op.writes & (1u << r)) usable_[r] = usable;
        if (op.writes_flags) flags_usable_ = usable;

        counts_.insns++;
        counts_.waited[int(why)] += slot - open;
        if (uses_alu(op)) {
            counts_.alu++;
            if (late) counts_.alu_late++;
        }
        if (op.reads_cond) {
            counts_.cond++;
            if (late) counts_.cond_late++;
            if (mispredicted) counts_.cond_mispredicted++;
            if (late && mispredicted) counts_.cond_late_mispredicted++;
        }
    }

    void clear_counts() { counts_ = Counts(); }

    void report(FILE* out) const {
        const Counts& c = counts_;
        auto n = [](uint64_t v) { return (unsigned long long)v; };
        uint64_t waited = 0;
        for (int i = 0; i < NEEDS; i++) waited += c.waited[i];
        uint64_t redirect = c.cond_late_mispredicted * late_offset();
        fprintf(out, "-- load-use %s\n", cfg_.label.c_str());
        fprintf(out, "  instructions %llu, waited %llu:", n(c.insns), n(waited));
        for (int i = 0; i < NEEDS; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", NEED_NAMES[i], n(c.waited[i]));
        fprintf(out, "\n  alu ops %llu, late %llu\n", n(c.alu), n(c.alu_late));
        fprintf(out, "  conditional branches %llu: late %llu, mispredicted %llu, both %llu\n",
                n(c.cond), n(c.cond_late), n(c.cond_mispredicted), n(c.cond_late_mispredicted));
        double k = c.insns / 1000.0;
        fprintf(out, "  cycles per 1000 instructions: %.1f (waits %.1f, late mispredictions %.1f)\n",
                k ? (waited + redirect) / k : 0.0, k ? waited / k : 0.0, k ? redirect / k : 0.0);
    }

private:
    // The classes an ALU executes.
    static bool uses_alu(const IssueOp& op) {
        return op.cls == IssueClass::Alu || op.cls == IssueClass::Shift
            || op.cls == IssueClass::Move || op.cls == IssueClass::Carry;
    }

    // The late stage's distance behind EX, in slots.
    uint64_t late_offset() const { return 1 + cfg_.wait; }

    // The first EX slot at which every register of `mask` is usable.
    uint64_t usable_at(uint16_t mask) const {
        uint64_t slot = 0;
        for (int r = 1; r < 15; r++)
            if (mask & (1u << r)) slot = std::max(slot, usable_[r]);
        return slot;
    }

    // Whether an op executes, or a conditional branch resolves, at the
    // late stage.  `open` is the first EX slot it can take, `ready` the
    // first at which what it reads is usable at EX.  With an ALU at both
    // stages, only an op that would wait at EX goes late.
    bool executes_late(uint64_t open, uint64_t ready) const {
        switch (cfg_.alu) {
        case AluPlacement::Ex:        return false;
        case AluPlacement::ExAndLate: return ready > open;
        default:                      return true;
        }
    }

    struct Counts {
        uint64_t insns = 0;
        uint64_t waited[NEEDS] = {};        // cycles waited, by the need they are charged to
        uint64_t alu = 0, alu_late = 0;     // ops an ALU executes; those of them at the late stage
        uint64_t cond = 0, cond_late = 0;   // conditional branches; those resolved at the late stage
        uint64_t cond_mispredicted = 0, cond_late_mispredicted = 0;
    };

    LoadUseConfig cfg_;
    uint64_t slot_ = 0;             // the EX slot the last instruction took
    uint64_t usable_[16] = {};      // by register: the first EX slot at which its value is usable
    uint64_t flags_usable_ = 0;     // likewise the flags
    Counts   counts_;
};

// ── The model: every pipeline +loadusemodel names ────────────────
class LoadUseModel {
public:
    // Parse the +loadusemodel value: empty for a default comparison, or a
    // comma-separated list of items.
    static bool parse(const char* spec, LoadUseModel& model, std::string& err) {
        std::string s(*spec ? spec : "ex,ex+late,late,ex+wait:1,ex+late+wait:1,late+wait:1");
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            LoadUseConfig c;
            if (!parse_item(item, c, err)) {
                err = "'" + item + "': " + err;
                return false;
            }
            model.pipelines_.emplace_back(c);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return true;
    }

    // One instruction that reached the end of its execution: `taken` when
    // it left the sequential path, `trapped` when it raised an exception.
    void insn(uint32_t pc, uint32_t word, bool taken, bool trapped) {
        if (trapped) return;
        IssueOp op = issue_decode(pc, word);
        bool mispredicted = op.reads_cond
            && front_end_.transfer(pc, BranchKind::Conditional, taken, branch_target(pc, word));
        for (auto& p : pipelines_) p.insn(op, mispredicted);
    }
    void clear_counts() { for (auto& p : pipelines_) p.clear_counts(); }
    void report(FILE* out) const { for (const auto& p : pipelines_) p.report(out); }

private:
    // Where a Format B branch at `pc` goes when taken.
    static uint32_t branch_target(uint32_t pc, uint32_t word) {
        int32_t offset = int32_t((word >> 4) & 0x3FFFFF);
        if (offset & 0x200000) offset |= int32_t(0xFFC00000);
        return pc + uint32_t(offset * 4);
    }

    static BpConfig front_end_config() {
        BpConfig c;
        c.label = "dec+bim1k";
        c.dir = 1024;
        c.decode = true;
        return c;
    }

    static bool parse_item(const std::string& item, LoadUseConfig& c, std::string& err) {
        c.label = item;
        size_t pos = 0;
        bool based = false;
        while (pos <= item.size()) {
            size_t plus = item.find('+', pos);
            std::string part = item.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            if (!based) {
                if (part == "ex")        c.alu = AluPlacement::Ex;
                else if (part == "late") c.alu = AluPlacement::Late;
                else { err = "an item starts with ex or late"; return false; }
                based = true;
            }
            else if (part == "late" && c.alu == AluPlacement::Ex) c.alu = AluPlacement::ExAndLate;
            else if (part.size() == 6 && part.compare(0, 5, "wait:") == 0
                     && part[5] >= '0' && part[5] <= '9')         c.wait = uint64_t(part[5] - '0');
            else {
                err = "unknown modifier '" + part + "' (late after ex, wait:N with N from 0 to 9)";
                return false;
            }
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
        return true;
    }

    std::vector<LoadUsePipeline> pipelines_;
    BranchPredictor front_end_{front_end_config()};     // says which conditional branches are mispredicted
};

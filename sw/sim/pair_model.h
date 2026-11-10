// pair_model.h — shadow two-slot issue stages (+pairmodel).
//
// Every instruction the ISS executes runs through one or more models of
// an in-order issue stage with two slots, X and Y, each counting the
// issue cycles the stream would take: which instructions issue as a
// pair, which fuse into one op, and what kept the rest apart.  The
// counts are exact for the architectural stream.  They are issue slots
// only: no stall, miss or misprediction is modelled, and every
// instruction is at hand when issue wants it unless the item gives the
// stage a fetch stage.
//
// An *op* is one instruction, or two adjacent ones a configured fusion
// combines.  Ops issue in order, one a cycle, or two when the younger
// may join the older:
//
//  - Sequence.  The younger directly follows the older: the older is not
//    a taken transfer, no exception was entered between them, and
//    neither trapped.  System instructions (WRSYS, RDSYS, WRSPR, RDSPR,
//    EI, DI, ERET, SYSCALL, BREAK) issue alone.
//  - Slots.  The older takes X and the younger Y, each slot issuing only
//    its classes; `flex` also allows the older in Y and the younger in X.
//  - Units.  One branch and one MUL/DIV a cycle, and one memory access
//    (`mem:ld2`: or two loads; `mem:2`: any two).
//  - Write ports.  Two register writes a cycle, so a fused load that also
//    writes its address (addwb, incmem) pairs only with an op that writes
//    none.  MUL and DIV count one: they sequence their second write.
//  - Dependencies.  The younger takes nothing the older writes that
//    cycle, a register or the flags, unless a configured forward
//    carries it.
//
// An item names one model: a base, `single` (X issues nothing) or `dual`
// (X issues alu, sh and mov; Y everything), then modifiers joined by '+':
//
//  - x:LIST / y:LIST: the slot's classes, joined by ':'.  alu (ADD SUB
//    AND OR XOR NOT CMP TEST), sh (SHL SHR SAR), mov (MOV LLI LLIS LUI),
//    adc (ADC SBC CMPC), ld, st, br (B Bcc BL JMP JALR), md (MUL DIV),
//    or the sets simple (alu:sh:mov), all and none.
//  - fw:LIST: same-cycle forwards from the older op to the younger.
//      cond   the flags it writes, to the condition a branch tests
//      carry  the flags it writes, to the carry ADC, SBC and CMPC add
//      mov    a MOV's source, wherever the younger reads the copy
//      imm    an LLI or LLIS immediate, or a fused LLI+LUI's, likewise
//      data   a result ready at the end of EX, as a store's data
//      ea     an ADD or SUB #imm result, as a load's or store's base (a
//             three-input address add)
//      alu    a result ready at the end of EX, as an ALU operand
//             (cascaded ALUs: a ceiling to measure against)
//  - fuse:LIST: fusions.
//      lilui  LLI Rd; LUI Rd as one load of a 32-bit constant
//      cmpbr  a flag-only compare; Bcc as one compare-and-branch
//      opbr   an ALU op that writes its register; Bcc as one
//             op-and-branch (no shift, and no ADC or SBC)
//      movop  MOV Rd, Rs; an ALU op or shift updating Rd in place, as
//             one three-operand op
//      ext    SHL Rd, #n; SHR or SAR Rd, #n as one zero or sign
//             extension of Rd's low 32 - n bits
//      shadd  SHL Rd, #1..3; ADD Rd, Rs as one add with Rd scaled on its
//             way into the adder (Rs not Rd)
//      addmem ADD Rd, Rs; a load or store with Rd as its base, as one
//             access through the sum of two registers.  A load fuses
//             only when it writes Rd, so the fused op writes one register
//      addwb  ADD Rd, Rs; a load through Rd into another register, as one
//             load that also writes its address back to Rd
//      incmem ADD or SUB Rd, #imm; a load or store with Rd as its base,
//             as one pre-incremented access; a load into another
//             register writes two, as addwb's does
//  - reach:any | reach:line | reach:pair: which adjacent instructions
//    the fuser sees.  Any two; two in one 16-byte line; or an aligned
//    pair, the first at an even word, as a fuser that looks inside each
//    fetch of two would.
//  - agu:3 | agu:2: the inputs of the address adder an address fusion
//    (addmem, addwb, incmem) uses.  Three takes the add's two and the
//    access's offset; two only the add's, so an access with an offset
//    does not fuse.  Three unless given.
//  - mem:1 | mem:ld2 | mem:2: memory accesses a cycle.
//  - flex: either op may take either slot.
//  - yflags: only the younger op of a pair commits flags.
//  - fetch:N | fetch:Nl: a fetch stage ahead of issue, delivering up to N
//    instructions a cycle into a queue; `l` also ends a fetch at the end
//    of its 16-byte line.
//  - bubble:N: cycles fetch idles after a redirect (1 unless given).
//  - queue:N: instructions the queue holds (8 unless given).
//
// A fused op has the class of its second instruction, except LLI+LUI,
// which stays a mov.  Fusions do not chain: an instruction is part of at
// most one, the first that matches in program order.
//
// With a fetch stage, a cycle issues from what the queue held when it
// began, and then fetches:
//
//  - Issue takes one op or a pair from the head of the queue and waits
//    for nothing that has not arrived.  Two instructions fuse only when
//    both are in the queue; an op with nothing behind it issues alone
//    ("fetch", among the singles with no candidate); and a cycle that
//    finds the queue empty is idle, and counts as an issue cycle.
//  - Fetch delivers one group, when the queue has room for a full one.
//    A group ends behind a taken transfer, and behind an instruction
//    that trapped or that an exception was entered behind.  Fetch is
//    then redirected, and idles for the bubble before the next group.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "issue_op.h"

// Sets of classes a slot can be given, a bit per IssueClass.
static constexpr uint32_t CLASSES_SIMPLE = 0x07;    // alu, sh, mov
static constexpr uint32_t CLASSES_ALL    = 0xFF;    // every class a slot can be given

// Same-cycle forwards (fw:), as bits of IssueConfig::forwards, from the
// narrowest to the widest: a pair counts a value several of them could
// carry under the first.
enum : uint32_t {
    FW_COND = 1 << 0, FW_CARRY = 1 << 1, FW_MOV = 1 << 2, FW_IMM = 1 << 3,
    FW_DATA  = 1 << 4, FW_EA    = 1 << 5, FW_ALU = 1 << 6,
};
static constexpr int FORWARDS = 7;
static const char* const FORWARD_NAMES[FORWARDS] =
    { "cond", "carry", "mov", "imm", "data", "ea", "alu" };

// Fusions (fuse:), as bits of IssueConfig::fusions.
enum : uint32_t {
    FUSE_LILUI = 1 << 0, FUSE_CMPBR = 1 << 1, FUSE_MOVOP = 1 << 2, FUSE_OPBR = 1 << 3,
    FUSE_EXT   = 1 << 4, FUSE_SHADD = 1 << 5, FUSE_ADDMEM = 1 << 6, FUSE_ADDWB = 1 << 7,
    FUSE_INCMEM = 1 << 8,
};
static constexpr int FUSIONS = 9;
static const char* const FUSION_NAMES[FUSIONS] =
    { "lilui", "cmpbr", "movop", "opbr", "ext", "shadd", "addmem", "addwb", "incmem" };

// The scale shadd's shift may apply on the way into the adder: the
// element sizes of halfword, word and doubleword arrays.
static constexpr int SHADD_MAX_SHIFT = 3;

// Which adjacent instructions the fuser sees (reach:): any two, two in
// one cache line, or an aligned pair.
enum class FuserReach { Any, Line, Pair };
static constexpr uint32_t ISSUE_LINE_WORDS = 4;     // a 16-byte line

// Why an op issued alone with no younger op to consider.  Fetch: the
// instruction behind it had not reached the queue.
enum class Alone { Taken, Break, System, End, Fetch };
static constexpr int ALONES = 5;
static const char* const ALONE_NAMES[ALONES] = { "taken", "break", "system", "end", "fetch" };

// The most instructions one cycle issues: two ops of two instructions.
static constexpr size_t CYCLE_INSNS = 4;
static const char* const CYCLE_INSNS_NAMES[CYCLE_INSNS] = { "one", "two", "three", "four" };

// What kept a younger op from joining, as bits: any number can hold at once.
enum : uint32_t {
    CAUSE_SLOT_OLDER = 1 << 0, CAUSE_SLOT_YOUNGER = 1 << 1, CAUSE_UNIT = 1 << 2,
    CAUSE_MEMORY     = 1 << 3, CAUSE_FLAGS        = 1 << 4, CAUSE_OPERAND = 1 << 5,
    CAUSE_BASE       = 1 << 6, CAUSE_DATA         = 1 << 7, CAUSE_TARGET  = 1 << 8,
    CAUSE_PORTS      = 1 << 9,
};
static constexpr int CAUSES = 10;
static const char* const CAUSE_NAMES[CAUSES] =
    { "slot-older", "slot-younger", "unit", "memory", "flags", "operand", "base", "data", "target",
      "ports" };

// Register writes a cycle: the register file's write ports.
static constexpr int WRITE_PORTS = 2;

// The words from an instruction to the end of its 16-byte line, its own
// among them.
static inline size_t issue_words_to_line_end(const IssueOp& op) {
    return ISSUE_LINE_WORDS - (op.pc / 4) % ISSUE_LINE_WORDS;
}

// Memory accesses a cycle may issue (mem:): one, two when both are
// loads, or any two.
enum class MemoryLimit { One, TwoLoads, Two };

// A first-in, first-out buffer of instructions, of a fixed capacity.
class InsnFifo {
public:
    explicit InsnFifo(size_t capacity) : slots_(capacity) {}
    size_t size() const { return count_; }
    bool   empty() const { return count_ == 0; }
    // The `i`-th oldest instruction held.
    const IssueOp& operator[](size_t i) const { return slots_[wrap(head_ + i)]; }
    IssueOp& newest() { return slots_[wrap(head_ + count_ - 1)]; }
    void push(const IssueOp& op) { slots_[wrap(head_ + count_++)] = op; }
    // Drop the `n` oldest.
    void pop(size_t n) { head_ = wrap(head_ + n); count_ -= n; }

private:
    size_t wrap(size_t i) const { return i < slots_.size() ? i : i - slots_.size(); }

    std::vector<IssueOp> slots_;
    size_t head_ = 0, count_ = 0;
};

// ── The fetch stage ──────────────────────────────────────────────
struct FetchConfig {
    size_t width = 1;           // fetch:N — the most instructions one fetch delivers
    bool   within_line = false; // fetch:Nl — a fetch also ends at the end of its 16-byte line
    size_t bubble = 1;          // bubble:N — cycles fetch idles after a redirect
    size_t queue = 8;           // queue:N — instructions the issue queue holds
};

class FetchStage {
public:
    explicit FetchStage(const FetchConfig& c) : cfg_(c), stream_(c.width + 1) {}

    // One executed instruction, in order.
    void insn(const IssueOp& op) { stream_.push(op); }

    // A cycle can run: the stream holds a full fetch and one instruction
    // more.  The newest instruction is never fetched while more may come,
    // since an exception may yet be entered behind it.
    bool ready() const { return stream_.size() > cfg_.width; }

    bool empty() const { return stream_.empty(); }

    // The instructions executed and not yet fetched, oldest first.
    InsnFifo& stream() { return stream_; }

    // One cycle's fetch, into the issue queue.
    void cycle(InsnFifo& queue) {
        if (bubble_left_) { bubble_left_--; counts_.bubbles++; return; }
        if (stream_.empty()) return;
        if (queue.size() + cfg_.width > cfg_.queue) { counts_.full++; return; }
        Group g = group();
        if (g.count < 1 || g.count > cfg_.width || g.count > stream_.size()) {
            fprintf(stderr, "pair_model: a fetch of %zu instructions, with a width of %zu and %zu to fetch\n",
                    g.count, cfg_.width, stream_.size());
            abort();
        }
        for (size_t i = 0; i < g.count; i++) queue.push(stream_[i]);
        stream_.pop(g.count);
        counts_.fetches++;
        if (g.redirected) bubble_left_ = cfg_.bubble;
    }

    void clear_counts() { counts_ = Counts(); }

    void report(FILE* out) const {
        auto n = [](uint64_t v) { return (unsigned long long)v; };
        fprintf(out, "  fetch: fetches %llu, bubbles %llu, full %llu\n",
                n(counts_.fetches), n(counts_.bubbles), n(counts_.full));
    }

private:
    // What one fetch delivers.
    struct Group {
        size_t count;       // instructions, taken in order from the oldest in the stream
        bool   redirected;  // the last of them leaves the path: fetch continues elsewhere
    };

    // The group this cycle's fetch delivers from the stream, which is not
    // empty.  A fetch reaches as far as its width and as the stream
    // holds, and with within_line no further than the end of its first
    // instruction's line: any adjacent words of a line make a group.  The
    // stream's instructions are consecutive in memory up to the first
    // that leaves the path, so the line's end is a count of them.  The
    // group ends sooner behind an instruction that leaves the path, which
    // is in it; fetch is then redirected.
    Group group() const {
        size_t reach = std::min(cfg_.width, stream_.size());
        if (cfg_.within_line) reach = std::min(reach, issue_words_to_line_end(stream_[0]));
        for (size_t i = 0; i < reach; i++)
            if (leaves_path(stream_[i])) return { i + 1, true };
        return { reach, false };
    }

    struct Counts {
        uint64_t fetches = 0;   // cycles a fetch delivered a group
        uint64_t bubbles = 0;   // cycles fetch idled after a redirect
        uint64_t full = 0;      // cycles the queue had no room for a full fetch
    };

    FetchConfig cfg_;
    InsnFifo    stream_;
    size_t      bubble_left_ = 0;   // cycles fetch still idles after the last redirect
    Counts      counts_;
};

// ── One issue stage ──────────────────────────────────────────────
struct IssueConfig {
    std::string label;
    uint32_t    x = 0, y = CLASSES_ALL;     // classes each slot issues, a bit per IssueClass
    uint32_t    forwards = 0;       // FW_* bits: the same-cycle forwards that exist
    uint32_t    fusions = 0;        // FUSE_* bits
    FuserReach  reach = FuserReach::Any;
    int         agu_inputs = 3;     // agu:N — the address adder's inputs in an address fusion
    MemoryLimit memory = MemoryLimit::One;
    bool        flex = false;       // the older op may take Y and the younger X
    bool        younger_commits_flags = false;  // yflags: the flag state takes only the younger op's write
    std::optional<FetchConfig> fetch;   // the fetch stage ahead of the queue, when the item names one
};

class IssueStage {
public:
    explicit IssueStage(IssueConfig c)
        : cfg_(std::move(c)), queue_(cfg_.fetch ? cfg_.fetch->queue : CYCLE_INSNS) {
        if (cfg_.fetch) fetch_.emplace(*cfg_.fetch);
    }

    // One executed instruction, in order.  With no fetch stage it is in
    // the queue at once, and a cycle runs when the queue holds as much as
    // one can issue: what the cycle does is then settled.
    void insn(const IssueOp& op) {
        if (fetch_) {
            fetch_->insn(op);
            while (fetch_->ready()) cycle();
        } else {
            queue_.push(op);
            while (queue_.size() == CYCLE_INSNS) cycle();
        }
    }

    // An exception was entered behind the newest instruction, which did
    // not raise it.
    void interrupted() {
        InsnFifo& held = fetch_ ? fetch_->stream() : queue_;
        if (!held.empty()) held.newest().interrupted = true;
    }

    // The window ends: what is waiting issues, and is counted in it.
    void end_window() { drain(); }

    void clear_counts() {
        drain();
        counts_ = Counts();
        if (fetch_) fetch_->clear_counts();
    }

    void report(FILE* out) const {
        const Counts& c = counts_;
        auto n = [](uint64_t v) { return (unsigned long long)v; };
        uint64_t cycles = c.pairs + c.singles + c.idle;
        fprintf(out, "-- pairing %s\n", cfg_.label.c_str());
        fprintf(out, "  instructions %llu, ops %llu, issue cycles %llu: %llu pairs, %llu singles, %llu idle\n",
                n(c.insns), n(c.ops), n(cycles), n(c.pairs), n(c.singles), n(c.idle));
        fprintf(out, "  issue cycles per 1000 instructions: %.1f\n",
                c.insns ? cycles * 1000.0 / c.insns : 0.0);
        fprintf(out, "  cycles by instructions issued:");
        for (size_t i = 0; i < CYCLE_INSNS; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", CYCLE_INSNS_NAMES[i], n(c.cycle_insns[i]));
        fprintf(out, "\n");
        if (fetch_) fetch_->report(out);
        fprintf(out, "  fused:");
        for (int i = 0; i < FUSIONS; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", FUSION_NAMES[i], n(c.fused[i]));
        fprintf(out, "\n  pairs by forward: none %llu", n(c.pair_plain));
        for (int i = 0; i < FORWARDS; i++) fprintf(out, ", %s %llu", FORWARD_NAMES[i], n(c.pair_forward[i]));
        fprintf(out, "\n  pairs by younger:");
        for (int i = 0; i < ISSUE_CLASSES - 1; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", ISSUE_CLASS_NAMES[i], n(c.pair_younger[i]));
        fprintf(out, "\n  singles by class:");
        for (int i = 0; i < ISSUE_CLASSES; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", ISSUE_CLASS_NAMES[i], n(c.single_class[i]));
        fprintf(out, "\n  singles, no candidate:");
        for (int i = 0; i < ALONES; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", ALONE_NAMES[i], n(c.single_alone[i]));
        fprintf(out, "\n  singles, one cause:");
        for (int i = 0; i < CAUSES; i++)
            fprintf(out, "%s %s %llu", i ? "," : "", CAUSE_NAMES[i], n(c.single_sole[i]));
        fprintf(out, "\n  singles, several causes: %llu (", n(c.single_several));
        for (int i = 0; i < CAUSES; i++)
            fprintf(out, "%s%s %llu", i ? ", " : "", CAUSE_NAMES[i], n(c.single_among[i]));
        fprintf(out, ")\n");
    }

private:
    static uint32_t class_bit(IssueClass c) { return 1u << int(c); }
    static bool accesses_memory(const IssueOp& op) {
        return op.cls == IssueClass::Load || op.cls == IssueClass::Store;
    }

    // `younger` is the instruction after `older` on the sequential path.
    static bool follows(const IssueOp& older, const IssueOp& younger) {
        return !leaves_path(older) && !younger.trapped
            && younger.pc == older.pc + 4 * uint32_t(older.insns);
    }

    // ── Fusion ──
    // An ALU op that writes its register and the flags: what a Bcc behind
    // it can fuse with.  A compare writes no register, and is cmpbr's.
    static bool feeds_branch(const IssueOp& op) {
        return op.cls == IssueClass::Alu && op.writes_flags && op.writes;
    }

    // `shl` is SHL Rd, #a and `shr` the SHR or SAR Rd, #b behind it.  Equal
    // distances are one extension of Rd from bit 31 - a: nothing moves,
    // and every bit above that one takes its value (SAR) or zero (SHR).
    // Unequal ones would need the shifter too.  A pair of #0 extends from
    // bit 31, which leaves Rd as it is and sets the flags: it is legal,
    // and excluding it would cost the decoder more than fusing it.
    static bool extends(const IssueOp& shl, const IssueOp& shr) {
        return shl.amount == shr.amount;
    }

    // SHL Rd, #1..3 and ADD Rd, Rs behind it, Rs another register: one ADD
    // whose Rd operand is scaled on its way into the adder.  With Rs as Rd
    // the ADD would read the shifted value twice.
    static bool scales_add(const IssueOp& shl, const IssueOp& add) {
        return shl.tied == TiedOp::ShlImm && add.tied == TiedOp::AddReg
            && add.rd == shl.rd && add.rs != shl.rd
            && shl.amount >= 1 && shl.amount <= SHADD_MAX_SHIFT;
    }

    // An add writing Rd and a load or store with Rd as its base: the
    // access whose address the add forms.
    static bool forms_base(const IssueOp& add, const IssueOp& access) {
        return add.writes && (access.reads[int(Use::Base)] & add.writes)
            && (access.cls == IssueClass::Load || access.cls == IssueClass::Store);
    }

    // The fused access would write the sum and a loaded value: a load into
    // a register other than the sum's.
    static bool writes_both(const IssueOp& add, const IssueOp& access) {
        return access.cls == IssueClass::Load && access.writes != add.writes;
    }

    // The access through the sum `add` forms, as one op: its base is the
    // add's registers, and it writes the sum unless its load overwrites
    // it.  What a fused load writes is ready when its value is.
    static void fuse_access(const IssueOp& add, const IssueOp& access, IssueOp& fused) {
        fused = access;
        fused.reads[int(Use::Base)] = add.reads[int(Use::Operand)];
        fused.reads[int(Use::Data)] &= ~add.writes;     // storing the sum: the op's own
        fused.writes = access.writes | add.writes;
        if (access.cls == IssueClass::Store) fused.produces = add.produces;
        fused.writes_flags = true;
    }

    // Register writes `op` needs ports for in its cycle.  MUL and DIV write
    // their second register a cycle later, through the same port.
    static int ports_used(const IssueOp& op) {
        if (op.cls == IssueClass::MulDiv) return op.writes ? 1 : 0;
        return __builtin_popcount(op.writes);
    }

    // The fuser sees `first` and the instruction behind it together.
    bool in_reach(const IssueOp& first) const {
        uint32_t word = first.pc >> 2;
        switch (cfg_.reach) {
        case FuserReach::Line: return word % ISSUE_LINE_WORDS != ISSUE_LINE_WORDS - 1;
        case FuserReach::Pair: return word % 2 == 0;
        default:               return true;
        }
    }

    bool fuses(uint32_t fusion) const { return (cfg_.fusions & fusion) != 0; }

    // The address adder has an input for `access`'s offset, or it needs none.
    bool agu_reaches(const IssueOp& access) const {
        return cfg_.agu_inputs == 3 || access.offset == 0;
    }

    // Two adjacent instructions the stage fuses, as the one op they make.
    // The first instruction alone does not say which fusion applies: an
    // ADD starts opbr or an address fusion, an SHL ext or shadd.
    bool fuse(const IssueOp& first, const IssueOp& second, IssueOp& fused) const {
        if (fuses(FUSE_LILUI) && first.shape == Shape::Lli && second.shape == Shape::Lui
            && first.rd == second.rd) {
            fused = first;                  // a constant, written to Rd
            fused.shape = Shape::Other;
            fused.fusion = 0;
        } else if (fuses(FUSE_CMPBR) && first.shape == Shape::Compare
                   && second.shape == Shape::CondBranch) {
            fused = second;                 // a branch that reads the compare's registers
            fused.reads[int(Use::Operand)] = first.reads[int(Use::Operand)];
            fused.writes_flags = true;
            fused.reads_cond = false;
            fused.fusion = 1;
        } else if (fuses(FUSE_MOVOP) && first.shape == Shape::Mov && second.shape == Shape::TiedAlu
                   && second.rd == first.rd && first.writes) {
            fused = second;                 // the op, reading the MOV's source in Rd's place
            fused.reads[int(Use::Operand)] =
                (second.reads[int(Use::Operand)] & ~first.writes) | first.reads[int(Use::Operand)];
            fused.fusion = 2;
        } else if (fuses(FUSE_OPBR) && feeds_branch(first) && second.shape == Shape::CondBranch) {
            fused = second;                 // a branch that also does the op
            fused.reads[int(Use::Operand)] = first.reads[int(Use::Operand)];
            fused.writes = first.writes;
            fused.produces = first.produces;
            fused.writes_flags = true;
            fused.reads_cond = false;
            fused.fusion = 3;
        } else if (fuses(FUSE_EXT) && first.tied == TiedOp::ShlImm && second.rd == first.rd
                   && (second.tied == TiedOp::ShrImm || second.tied == TiedOp::SarImm)
                   && extends(first, second)) {
            fused = second;                 // one shift of Rd that fills its upper bits
            fused.fusion = 4;
        } else if (fuses(FUSE_SHADD) && scales_add(first, second)) {
            fused = second;                 // the ADD, reading Rd before the shift
            fused.fusion = 5;
        } else if (fuses(FUSE_ADDMEM) && first.tied == TiedOp::AddReg && forms_base(first, second)
                   && !writes_both(first, second) && agu_reaches(second)) {
            fuse_access(first, second, fused);
            fused.fusion = 6;
        } else if (fuses(FUSE_ADDWB) && first.tied == TiedOp::AddReg && forms_base(first, second)
                   && writes_both(first, second) && agu_reaches(second)) {
            fuse_access(first, second, fused);
            fused.fusion = 7;
        } else if (fuses(FUSE_INCMEM) && first.tied == TiedOp::AddSubImm && forms_base(first, second)
                   && agu_reaches(second)) {
            fuse_access(first, second, fused);
            fused.fusion = 8;
        } else {
            return false;
        }
        fused.pc = first.pc;
        fused.insns = 2;
        // What is executed after the fused op is what follows its second
        // instruction.
        fused.taken = second.taken;
        fused.interrupted = second.interrupted;
        return true;
    }

    // The op that starts at the queue's `at`-th instruction: that
    // instruction, or it fused with the one behind it when that one is in
    // the queue.
    IssueOp op_at(size_t at) const {
        const IssueOp& first = queue_[at];
        IssueOp fused;
        if (at + 1 < queue_.size() && in_reach(first) && follows(first, queue_[at + 1])
            && fuse(first, queue_[at + 1], fused))
            return fused;
        return first;
    }

    // ── Pairing ──
    struct Verdict {
        bool     pairs = false;
        Alone    alone = Alone::Break;  // when the younger was no candidate at all
        bool     candidate = false;
        uint32_t causes = 0;            // CAUSE_* bits, when it was a candidate and did not join
        uint32_t forwards = 0;          // FW_* bits a pair relies on
    };

    // ── What a pair needs ──
    // These describe the two ops alone.  Whether a stage supplies what
    // they describe is judge()'s question.

    // One value `younger` takes from `older` in the cycle they would share.
    struct Link {
        uint32_t cause;     // the CAUSE_* bit charged when no configured forward carries it
        uint32_t forwards;  // FW_* bits, any one of which carries it; 0 when none can
    };
    static constexpr int MAX_LINKS = USES + 2;

    // The forwards that carry a value produced as `p` to a use `u` in the
    // cycle it is produced.
    static uint32_t forwards_carrying(Producer p, Use u) {
        if (p == Producer::Late) return 0;
        uint32_t fw = 0;
        if (p == Producer::Copy)     fw |= FW_MOV;
        if (p == Producer::Constant) fw |= FW_IMM;
        if (u == Use::Data)          fw |= FW_DATA;
        if (u == Use::Base && p == Producer::AddImm) fw |= FW_EA;
        if (u == Use::Operand)       fw |= FW_ALU;
        return fw;
    }

    // Every value `younger` takes from `older`, the op directly ahead of
    // it: each register the older op writes and the younger reads, by
    // use, and the flags the older op writes, as the younger's condition
    // or carry.  Flags that ops of earlier cycles wrote are no link: they
    // stand by the time the younger op executes.  Returns how many links
    // `out` holds.
    static int links(const IssueOp& older, const IssueOp& younger, Link out[MAX_LINKS]) {
        static constexpr uint32_t use_cause[USES] =
            { CAUSE_OPERAND, CAUSE_BASE, CAUSE_DATA, CAUSE_TARGET };
        int count = 0;
        for (int u = 0; u < USES; u++)
            if (older.writes & younger.reads[u])
                out[count++] = { use_cause[u], forwards_carrying(older.produces, Use(u)) };
        if (older.writes_flags && younger.reads_cond)  out[count++] = { CAUSE_FLAGS, FW_COND };
        if (older.writes_flags && younger.reads_carry) out[count++] = { CAUSE_FLAGS, FW_CARRY };
        return count;
    }

    // The flags the pair leaves are the older op's: after a pair the
    // flag state is what its youngest flag writer left.
    static bool leaves_older_flags(const IssueOp& older, const IssueOp& younger) {
        return older.writes_flags && !younger.writes_flags;
    }

    // ── Whether this stage issues it ──
    Verdict judge(const IssueOp& older, const IssueOp& younger) const {
        Verdict v;
        if (older.trapped || older.interrupted || younger.trapped)
                                                     { v.alone = Alone::Break;  return v; }
        if (older.cls == IssueClass::System || younger.cls == IssueClass::System)
                                                     { v.alone = Alone::System; return v; }
        if (older.taken)                             { v.alone = Alone::Taken;  return v; }
        if (!follows(older, younger))                { v.alone = Alone::Break;  return v; }
        v.candidate = true;

        uint32_t a = class_bit(older.cls), b = class_bit(younger.cls);
        bool fits = ((cfg_.x & a) && (cfg_.y & b)) || (cfg_.flex && (cfg_.y & a) && (cfg_.x & b));
        if (!fits) {
            if (!(cfg_.x & a)) v.causes |= CAUSE_SLOT_OLDER;
            if (!(cfg_.y & b)) v.causes |= CAUSE_SLOT_YOUNGER;
        }

        if (older.cls == younger.cls
            && (older.cls == IssueClass::Branch || older.cls == IssueClass::MulDiv))
            v.causes |= CAUSE_UNIT;

        if (accesses_memory(older) && accesses_memory(younger)) {
            bool both_load = older.cls == IssueClass::Load && younger.cls == IssueClass::Load;
            if (cfg_.memory == MemoryLimit::One || (cfg_.memory == MemoryLimit::TwoLoads && !both_load))
                v.causes |= CAUSE_MEMORY;
        }

        if (ports_used(older) + ports_used(younger) > WRITE_PORTS)
            v.causes |= CAUSE_PORTS;

        Link link[MAX_LINKS];
        int count = links(older, younger, link);
        for (int i = 0; i < count; i++) {
            uint32_t carrying = link[i].forwards & cfg_.forwards;
            if (carrying) v.forwards |= carrying & (~carrying + 1);    // the narrowest
            else          v.causes |= link[i].cause;
        }

        if (cfg_.younger_commits_flags && leaves_older_flags(older, younger))
            v.causes |= CAUSE_FLAGS;

        v.pairs = !v.causes;
        return v;
    }

    // Why an op issues alone when the queue holds nothing behind it.
    Alone alone_at_tail(const IssueOp& op) const {
        if (!fetch_ || fetch_->empty())     return Alone::End;      // nothing more was executed
        if (op.trapped || op.interrupted)   return Alone::Break;
        if (op.cls == IssueClass::System)   return Alone::System;
        if (op.taken)                       return Alone::Taken;
        return Alone::Fetch;
    }

    // ── A cycle ──
    // Issue from what the queue holds, then fetch into it.
    void cycle() {
        if (queue_.empty()) counts_.idle++;
        else                queue_.pop(issue());
        if (fetch_) fetch_->cycle(queue_);
    }

    // Issue one op, or a pair, from the head of the queue.  Returns the
    // instructions that leave it.
    size_t issue() {
        IssueOp older = op_at(0);
        size_t issued = older.insns;
        if (issued == queue_.size()) {
            count_single(older);
            counts_.single_alone[int(alone_at_tail(older))]++;
        } else {
            IssueOp younger = op_at(issued);
            Verdict v = judge(older, younger);
            if (v.pairs) {
                count_pair(older, younger, v);
                issued += younger.insns;
            } else {
                count_single(older);
                count_kept_out(v);
            }
        }
        counts_.cycle_insns[issued - 1]++;
        return issued;
    }

    // Run until everything executed has issued.
    void drain() {
        while (!queue_.empty() || (fetch_ && !fetch_->empty())) cycle();
    }

    void count_op(const IssueOp& op) {
        counts_.insns += op.insns;
        counts_.ops++;
        if (op.fusion >= 0) counts_.fused[op.fusion]++;
    }

    void count_pair(const IssueOp& older, const IssueOp& younger, const Verdict& v) {
        count_op(older);
        count_op(younger);
        counts_.pairs++;
        counts_.pair_younger[int(younger.cls)]++;
        if (!v.forwards) counts_.pair_plain++;
        for (int i = 0; i < FORWARDS; i++)
            if (v.forwards & (1u << i)) counts_.pair_forward[i]++;
    }

    void count_single(const IssueOp& op) {
        count_op(op);
        counts_.singles++;
        counts_.single_class[int(op.cls)]++;
    }

    // A single that the op behind it did not join: why.
    void count_kept_out(const Verdict& v) {
        if (!v.candidate) {
            counts_.single_alone[int(v.alone)]++;
        } else if (v.causes & (v.causes - 1)) {
            counts_.single_several++;
            for (int i = 0; i < CAUSES; i++)
                if (v.causes & (1u << i)) counts_.single_among[i]++;
        } else {
            counts_.single_sole[__builtin_ctz(v.causes)]++;
        }
    }

    struct Counts {
        uint64_t insns = 0, ops = 0, pairs = 0, singles = 0;
        uint64_t idle = 0;                      // cycles that found the queue empty
        uint64_t cycle_insns[CYCLE_INSNS] = {}; // cycles by instructions issued, from one up
        uint64_t fused[FUSIONS] = {};
        uint64_t pair_plain = 0, pair_forward[FORWARDS] = {};   // a pair counts under each forward it uses
        uint64_t pair_younger[ISSUE_CLASSES] = {}, single_class[ISSUE_CLASSES] = {};
        uint64_t single_alone[ALONES] = {};
        uint64_t single_sole[CAUSES] = {};      // exactly one cause held
        uint64_t single_several = 0, single_among[CAUSES] = {};  // several held; each counted
    };

    IssueConfig cfg_;
    InsnFifo    queue_;                 // instructions issue can take, oldest first
    std::optional<FetchStage> fetch_;   // what fills the queue, when the item names a fetch stage
    Counts      counts_;
};

// ── The model: every stage +pairmodel names ──────────────────────
class PairModel {
public:
    // Parse the +pairmodel value: empty for a default comparison, or a
    // comma-separated list of items, each `single` or `dual` followed by
    // modifiers joined by '+'.
    static bool parse(const char* spec, PairModel& model, std::string& err) {
        std::string s(*spec ? spec
            : "single,single+fuse:lilui,dual+yflags,dual,dual+fw:cond,"
              "dual+fw:cond:mov:imm:data,dual+fw:cond:mov:imm:data+fuse:lilui,"
              "dual+fw:cond:mov:imm:data+fuse:lilui+flex,"
              "dual+x:simple:ld+fw:cond:mov:imm:data+fuse:lilui+flex+mem:ld2");
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            IssueConfig c;
            if (!parse_item(item, c, err)) {
                err = "'" + item + "': " + err;
                return false;
            }
            model.stages_.emplace_back(c);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return true;
    }

    // One instruction that reached the end of its execution: `taken` when
    // it left the sequential path, `trapped` when it raised an exception.
    void insn(uint32_t pc, uint32_t word, bool taken, bool trapped) {
        IssueOp op = issue_decode(pc, word);
        op.taken = taken;
        op.trapped = trapped;
        // An exception entered since the last instruction, other than the
        // one this instruction raised, was entered behind the last one.
        bool behind_last = exceptions_ > (trapped ? 1u : 0u);
        exceptions_ = 0;
        for (auto& s : stages_) {
            if (behind_last) s.interrupted();
            s.insn(op);
        }
    }
    // An exception is entered, a boundary in the stream: an interrupt, a
    // fault ahead of an instruction's execution, or the trap of the
    // instruction that insn() brings next.
    void boundary() { exceptions_++; }
    void end_window() { for (auto& s : stages_) s.end_window(); }
    void clear_counts() { for (auto& s : stages_) s.clear_counts(); }
    void report(FILE* out) const { for (const auto& s : stages_) s.report(out); }

private:
    // Split `list` at each ':' and hand the parts to `each`, which
    // returns false for one it does not know.
    template <typename F>
    static bool for_each_part(const std::string& list, F each) {
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t colon = list.find(':', pos);
            if (!each(list.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos)))
                return false;
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
        return true;
    }

    static bool named_bit(const std::string& name, const char* const names[], int count, uint32_t& bits) {
        for (int i = 0; i < count; i++)
            if (name == names[i]) { bits |= 1u << i; return true; }
        return false;
    }

    static bool parse_classes(const std::string& list, uint32_t& classes) {
        classes = 0;
        return for_each_part(list, [&](const std::string& part) {
            if (part == "none")   return true;
            if (part == "simple") { classes |= CLASSES_SIMPLE; return true; }
            if (part == "all")    { classes |= CLASSES_ALL;    return true; }
            return named_bit(part, ISSUE_CLASS_NAMES, ISSUE_CLASSES - 1, classes);
        });
    }

    // A decimal count of at least `least`.
    static bool parse_count(const std::string& text, size_t least, size_t& count) {
        if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos)
            return false;
        count = std::stoul(text);
        return count >= least;
    }

    static bool parse_item(const std::string& item, IssueConfig& c, std::string& err) {
        c.label = item;
        size_t pos = 0;
        bool based = false;
        FetchConfig fetch;
        bool fetches = false, shapes_fetch = false;     // fetch: was given; bubble: or queue: was
        while (pos <= item.size()) {
            size_t plus = item.find('+', pos);
            std::string part = item.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            auto list = [&](const char* prefix) {
                size_t len = strlen(prefix);
                return part.compare(0, len, prefix) == 0 ? part.substr(len) : std::string();
            };
            bool ok = true;
            if (!based) {
                if (part == "single")    c.x = 0;
                else if (part == "dual") c.x = CLASSES_SIMPLE;
                else { err = "an item starts with single or dual"; return false; }
                based = true;
            }
            else if (part == "flex")            c.flex = true;
            else if (part == "yflags")          c.younger_commits_flags = true;
            else if (part == "mem:1")           c.memory = MemoryLimit::One;
            else if (part == "mem:ld2")         c.memory = MemoryLimit::TwoLoads;
            else if (part == "mem:2")           c.memory = MemoryLimit::Two;
            else if (part == "reach:any")       c.reach = FuserReach::Any;
            else if (part == "reach:line")      c.reach = FuserReach::Line;
            else if (part == "reach:pair")      c.reach = FuserReach::Pair;
            else if (part == "agu:3")           c.agu_inputs = 3;
            else if (part == "agu:2")           c.agu_inputs = 2;
            else if (!list("x:").empty())       ok = parse_classes(list("x:"), c.x);
            else if (!list("y:").empty())       ok = parse_classes(list("y:"), c.y);
            else if (!list("fw:").empty())
                ok = for_each_part(list("fw:"), [&](const std::string& name) {
                    return named_bit(name, FORWARD_NAMES, FORWARDS, c.forwards);
                });
            else if (!list("fuse:").empty())
                ok = for_each_part(list("fuse:"), [&](const std::string& name) {
                    return named_bit(name, FUSION_NAMES, FUSIONS, c.fusions);
                });
            else if (!list("fetch:").empty()) {
                std::string width = list("fetch:");
                fetch.within_line = width.back() == 'l';
                if (fetch.within_line) width.pop_back();
                ok = parse_count(width, 1, fetch.width);
                fetches = true;
            }
            else if (!list("bubble:").empty()) {
                ok = parse_count(list("bubble:"), 0, fetch.bubble);
                shapes_fetch = true;
            }
            else if (!list("queue:").empty()) {
                ok = parse_count(list("queue:"), 1, fetch.queue);
                shapes_fetch = true;
            }
            else ok = false;
            if (!ok) {
                err = "unknown modifier '" + part + "' (x:CLASSES, y:CLASSES, fw:FORWARDS,"
                      " fuse:FUSIONS, reach:any|line|pair, agu:3|2, mem:1|ld2|2, flex, yflags,"
                      " fetch:N[l], bubble:N, queue:N)";
                return false;
            }
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
        if (shapes_fetch && !fetches) {
            err = "bubble: and queue: shape a fetch stage, and need fetch:";
            return false;
        }
        if (fetches && fetch.queue < fetch.width) {
            err = "the queue holds less than one fetch delivers";
            return false;
        }
        if (fetches) c.fetch = fetch;
        return true;
    }

    std::vector<IssueStage> stages_;
    unsigned exceptions_ = 0;   // exceptions entered since the last instruction
};

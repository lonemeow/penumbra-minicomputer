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
//  - reach:any | reach:line | reach:pair: which adjacent instructions
//    the fuser sees.  Any two; two in one 16-byte line; or an aligned
//    pair, the first at an even word, as a fuser that looks inside each
//    fetch of two would.
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

// What a slot must be able to do to issue an instruction.
enum class IssueClass { Alu, Shift, Move, Carry, Load, Store, Branch, MulDiv, System };
static constexpr int ISSUE_CLASSES = 9;
static const char* const ISSUE_CLASS_NAMES[ISSUE_CLASSES] =
    { "alu", "sh", "mov", "adc", "ld", "st", "br", "md", "sys" };
static constexpr uint32_t CLASSES_SIMPLE = 0x07;    // alu, sh, mov
static constexpr uint32_t CLASSES_ALL    = 0xFF;    // every class a slot can be given

// How an op uses a register it reads, which sets when it needs the value.
enum class Use { Operand, Base, Data, Target };     // ALU input; address base; store data; jump target
static constexpr int USES = 4;

// How the value an op writes comes about, which sets when it exists.
enum class Producer {
    Copy,       // a MOV: its source register's value
    Constant,   // known at decode: an immediate, or a call's return address
    AddImm,     // a register plus or minus an immediate, at the end of EX
    Execute,    // any other result ready at the end of EX
    Late,       // after EX: a load, MUL or DIV
};

// The shapes the fusions look for.
enum class Shape { Other, Lli, Lui, Mov, Compare, TiedAlu, CondBranch };

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
};
static constexpr int FUSIONS = 4;
static const char* const FUSION_NAMES[FUSIONS] = { "lilui", "cmpbr", "movop", "opbr" };

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
};
static constexpr int CAUSES = 9;
static const char* const CAUSE_NAMES[CAUSES] =
    { "slot-older", "slot-younger", "unit", "memory", "flags", "operand", "base", "data", "target" };

// One instruction, or a fused pair, as the issue stage sees it.
struct IssueOp {
    uint32_t   pc = 0;              // address of its first instruction
    int        insns = 1;           // instructions it stands for: 2 when fused
    IssueClass cls = IssueClass::System;
    uint16_t   writes = 0;          // registers it writes, a bit each; never R0 or PC
    uint16_t   reads[USES] = {};    // registers it reads, a bit each, by use; never R0 or PC
    Producer   produces = Producer::Late;   // how the registers it writes get their value
    bool       writes_flags = false;        // it leaves new NZCV
    bool       reads_cond = false;  // it branches on flags an earlier op wrote (a fused
                                    // compare-and-branch tests its own, so does not)
    bool       reads_carry = false; // ADC, SBC or CMPC: it adds in the C an earlier op wrote
    bool       taken = false;       // a control transfer that left the sequential path
    bool       trapped = false;     // it raised an exception instead of completing
    bool       interrupted = false; // an exception it did not raise was entered behind it: an
                                    // interrupt, or a fault ahead of the next instruction's execution
    Shape      shape = Shape::Other;
    int        rd = 0, rs = 0;      // register fields, for the shapes that name them
    int        fusion = -1;         // the fusion that made it, an index into FUSION_NAMES; -1 when not fused
};

static inline uint16_t issue_reg_bit(int r) { return (r == 0 || r == 15) ? 0 : uint16_t(1u << r); }

// The instruction executed after `op` is not the one behind it in memory.
static inline bool leaves_path(const IssueOp& op) { return op.taken || op.trapped || op.interrupted; }

// The words from an instruction to the end of its 16-byte line, its own
// among them.
static inline size_t issue_words_to_line_end(const IssueOp& op) {
    return ISSUE_LINE_WORDS - (op.pc / 4) % ISSUE_LINE_WORDS;
}

// The op one instruction word makes, before the run says whether it was
// taken or trapped.
static inline IssueOp issue_decode(uint32_t pc, uint32_t word) {
    IssueOp op;
    op.pc = pc;
    auto operand = [&](int r) { op.reads[int(Use::Operand)] |= issue_reg_bit(r); };
    switch (word >> 30) {
    case 0: {   // Format R
        int code = (word >> 25) & 0x1F;
        bool flag_only = (word >> 16) & 1;
        op.rd = (word >> 21) & 0xF;
        op.rs = (word >> 17) & 0xF;
        if (code <= 7) {                    // ADD SUB AND OR XOR SHL SHR SAR
            op.cls = code >= 5 ? IssueClass::Shift : IssueClass::Alu;
            operand(op.rd); operand(op.rs);
            op.writes_flags = true;
            op.produces = Producer::Execute;
            op.shape = flag_only ? Shape::Compare : Shape::TiedAlu;
            if (!flag_only) op.writes = issue_reg_bit(op.rd);
        } else if (code == 8) {             // MOV
            op.cls = IssueClass::Move;
            operand(op.rs);
            op.produces = Producer::Copy;
            if (!flag_only) { op.writes = issue_reg_bit(op.rd); op.shape = Shape::Mov; }
        } else if (code == 9) {             // NOT
            op.cls = IssueClass::Alu;
            operand(op.rs);
            op.writes_flags = true;
            op.produces = Producer::Execute;
            if (!flag_only) op.writes = issue_reg_bit(op.rd);
        } else if (code <= 11) {            // ADC SBC, and CMPC with the F bit
            op.cls = IssueClass::Carry;
            operand(op.rd); operand(op.rs);
            op.reads_carry = op.writes_flags = true;
            op.produces = Producer::Execute;
            if (!flag_only) op.writes = issue_reg_bit(op.rd);
        } else if (code >= 16 && code <= 19) {  // MUL MULU DIV DIVU
            op.cls = IssueClass::MulDiv;
            operand(op.rd); operand(op.rs);
            op.writes_flags = true;
            op.writes = issue_reg_bit(op.rd) | issue_reg_bit((word >> 12) & 0xF);
        }
        break;
    }
    case 1: {   // Format L
        int code = (word >> 26) & 0xF;
        op.rd = (word >> 22) & 0xF;
        if (code <= 1) {                    // LLI LLIS
            op.cls = IssueClass::Move;
            op.writes = issue_reg_bit(op.rd);
            op.produces = Producer::Constant;
            if (code == 0) op.shape = Shape::Lli;
        } else if (code == 2) {             // LUI
            op.cls = IssueClass::Move;
            operand(op.rd);
            op.writes = issue_reg_bit(op.rd);
            op.produces = Producer::Execute;
            op.shape = Shape::Lui;
        } else if (code <= 10) {            // ADD SUB CMP AND TEST SHL SHR SAR #imm
            bool flag_only = code == 5 || code == 7;
            op.cls = code >= 8 ? IssueClass::Shift : IssueClass::Alu;
            operand(op.rd);
            op.writes_flags = true;
            op.produces = code <= 4 ? Producer::AddImm : Producer::Execute;
            op.shape = flag_only ? Shape::Compare : Shape::TiedAlu;
            if (!flag_only) op.writes = issue_reg_bit(op.rd);
        } else if (code <= 12) {            // JMP JALR
            op.cls = IssueClass::Branch;
            op.reads[int(Use::Target)] = issue_reg_bit(op.rd);
            if (code == 12) { op.writes = issue_reg_bit(13); op.produces = Producer::Constant; }
        }
        break;
    }
    case 2: {   // Format M
        op.rd = (word >> 22) & 0xF;
        op.reads[int(Use::Base)] = issue_reg_bit((word >> 18) & 0xF);
        if ((word >> 29) & 1) {
            op.cls = IssueClass::Load;
            op.writes = issue_reg_bit(op.rd);
        } else {
            op.cls = IssueClass::Store;
            op.reads[int(Use::Data)] = issue_reg_bit(op.rd);
        }
        break;
    }
    default: {  // Format B
        int cond = (word >> 26) & 0xF;
        op.cls = IssueClass::Branch;
        if (cond == 15) { op.writes = issue_reg_bit(13); op.produces = Producer::Constant; }
        else if (cond != 0) { op.reads_cond = true; op.shape = Shape::CondBranch; }
        break;
    }
    }
    return op;
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

    // The fuser sees `first` and the instruction behind it together.
    bool in_reach(const IssueOp& first) const {
        uint32_t word = first.pc >> 2;
        switch (cfg_.reach) {
        case FuserReach::Line: return word % ISSUE_LINE_WORDS != ISSUE_LINE_WORDS - 1;
        case FuserReach::Pair: return word % 2 == 0;
        default:               return true;
        }
    }

    bool starts_fusion(const IssueOp& op) const {
        if (op.trapped || !in_reach(op)) return false;
        return (op.shape == Shape::Lli && (cfg_.fusions & FUSE_LILUI))
            || (op.shape == Shape::Compare && (cfg_.fusions & FUSE_CMPBR))
            || (op.shape == Shape::Mov && (cfg_.fusions & FUSE_MOVOP))
            || (feeds_branch(op) && (cfg_.fusions & FUSE_OPBR));
    }

    bool fuse(const IssueOp& first, const IssueOp& second, IssueOp& fused) const {
        if (first.shape == Shape::Lli && second.shape == Shape::Lui && first.rd == second.rd) {
            fused = first;                  // a constant, written to Rd
            fused.shape = Shape::Other;
            fused.fusion = 0;
        } else if (first.shape == Shape::Compare && second.shape == Shape::CondBranch) {
            fused = second;                 // a branch that reads the compare's registers
            fused.reads[int(Use::Operand)] = first.reads[int(Use::Operand)];
            fused.writes_flags = true;
            fused.reads_cond = false;
            fused.fusion = 1;
        } else if (first.shape == Shape::Mov && second.shape == Shape::TiedAlu
                   && second.rd == first.rd && first.writes) {
            fused = second;                 // the op, reading the MOV's source in Rd's place
            fused.reads[int(Use::Operand)] =
                (second.reads[int(Use::Operand)] & ~first.writes) | first.reads[int(Use::Operand)];
            fused.fusion = 2;
        } else if (feeds_branch(first) && second.shape == Shape::CondBranch) {
            fused = second;                 // a branch that also does the op
            fused.reads[int(Use::Operand)] = first.reads[int(Use::Operand)];
            fused.writes = first.writes;
            fused.produces = first.produces;
            fused.writes_flags = true;
            fused.reads_cond = false;
            fused.fusion = 3;
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
        if (at + 1 < queue_.size() && starts_fusion(first) && follows(first, queue_[at + 1])
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
                      " fuse:FUSIONS, reach:any|line|pair, mem:1|ld2|2, flex, yflags,"
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

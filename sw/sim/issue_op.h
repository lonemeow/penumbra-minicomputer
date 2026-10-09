// issue_op.h — an instruction as a pipeline's issue stage sees it.
//
// What the shadow pipeline models (pair_model.h, loaduse_model.h) need
// of an instruction to say when it can issue: the kind of unit it needs,
// the registers it reads and how it uses each, and the registers and
// flags it writes and how their values come about.

#pragma once

#include <cstdint>

// The kind of unit an instruction needs to execute.
enum class IssueClass { Alu, Shift, Move, Carry, Load, Store, Branch, MulDiv, System };
static constexpr int ISSUE_CLASSES = 9;
static const char* const ISSUE_CLASS_NAMES[ISSUE_CLASSES] =
    { "alu", "sh", "mov", "adc", "ld", "st", "br", "md", "sys" };

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

// The shapes the pairing model's fusions look for.
enum class Shape { Other, Lli, Lui, Mov, Compare, TiedAlu, CondBranch };

// One instruction, or a pair the pairing model fused, as an issue stage
// sees it.
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
    int        fusion = -1;         // the fusion that made it, as the pairing model numbers them; -1 when not fused
};

static inline uint16_t issue_reg_bit(int r) { return (r == 0 || r == 15) ? 0 : uint16_t(1u << r); }

// The instruction executed after `op` is not the one behind it in memory.
static inline bool leaves_path(const IssueOp& op) { return op.taken || op.trapped || op.interrupted; }

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

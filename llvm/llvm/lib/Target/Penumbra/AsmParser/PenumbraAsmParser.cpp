//===-- PenumbraAsmParser.cpp - Parse Penumbra assembly to MCInst ---------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PenumbraFixupKinds.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <memory>

using namespace llvm;

// Forward-declare TableGen-generated register matcher (defined at end of file).
static MCRegister MatchRegisterName(StringRef Name);

namespace {

//===----------------------------------------------------------------------===//
// Operand representation
//===----------------------------------------------------------------------===//

class PenumbraOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate } Kind;

  SMLoc StartLoc, EndLoc;

  struct TokOp {
    const char *Data;
    unsigned Length;
  };
  struct RegOp {
    MCRegister Reg;
  };
  struct ImmOp {
    const MCExpr *Val;
  };

  union {
    TokOp Tok;
    RegOp RegInfo;
    ImmOp Imm;
  };

public:
  PenumbraOperand(KindTy K) : Kind(K) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  MCRegister getReg() const override {
    assert(Kind == Register && "Not a register");
    return RegInfo.Reg;
  }

  StringRef getToken() const {
    assert(Kind == Token && "Not a token");
    return StringRef(Tok.Data, Tok.Length);
  }

  const MCExpr *getImmVal() const {
    assert(Kind == Immediate && "Not an immediate");
    return Imm.Val;
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    if (auto *CE = llvm::dyn_cast<MCConstantExpr>(getImmVal()))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(getImmVal()));
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Token:
      OS << "Tok:" << getToken();
      break;
    case Register:
      OS << "Reg:" << getReg();
      break;
    case Immediate:
      OS << "Imm:<expr>";
      break;
    }
  }

  static std::unique_ptr<PenumbraOperand> createToken(StringRef Str,
                                                      SMLoc S) {
    auto Op = std::make_unique<PenumbraOperand>(Token);
    Op->Tok.Data = Str.data();
    Op->Tok.Length = Str.size();
    Op->StartLoc = S;
    Op->EndLoc = S;
    return Op;
  }

  static std::unique_ptr<PenumbraOperand> createReg(MCRegister Reg, SMLoc S,
                                                    SMLoc E) {
    auto Op = std::make_unique<PenumbraOperand>(Register);
    Op->RegInfo.Reg = Reg;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<PenumbraOperand> createImm(const MCExpr *Val, SMLoc S,
                                                    SMLoc E) {
    auto Op = std::make_unique<PenumbraOperand>(Immediate);
    Op->Imm.Val = Val;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
};

//===----------------------------------------------------------------------===//
// Assembly parser
//===----------------------------------------------------------------------===//

class PenumbraAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

  bool parseOperand(OperandVector &Operands);
  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;
  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;
  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  /// Expand LI (load immediate) pseudo: LI Rd, #imm32
  bool emitLoadImmediate(SMLoc IDLoc, OperandVector &Operands, MCStreamer &Out);
  /// Expand LA (load address) pseudo: LA Rd, symbol
  bool emitLoadAddress(SMLoc IDLoc, OperandVector &Operands, MCStreamer &Out);
  /// Emit an LLI+LUI pair for a 32-bit symbolic expression with lo16/hi16.
  void emitLLI_LUI_Pair(MCRegister Rd, const MCExpr *Expr, SMLoc Loc,
                         MCStreamer &Out);

  // Auto-generated by TableGen.
  unsigned validateTargetOperandClass(MCParsedAsmOperand &Op,
                                      unsigned Kind) override;

#define GET_ASSEMBLER_HEADER
#include "PenumbraGenAsmMatcher.inc"

public:
  PenumbraAsmParser(const MCSubtargetInfo &STI, MCAsmParser &P,
                    const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(P) {
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Register parsing
//===----------------------------------------------------------------------===//

bool PenumbraAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                      SMLoc &EndLoc) {
  return tryParseRegister(Reg, StartLoc, EndLoc).isFailure();
}

ParseStatus PenumbraAsmParser::tryParseRegister(MCRegister &Reg,
                                                SMLoc &StartLoc,
                                                SMLoc &EndLoc) {
  StartLoc = Parser.getTok().getLoc();
  if (Parser.getTok().isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  StringRef Name = Parser.getTok().getIdentifier();
  MCRegister RegNo = MatchRegisterName(Name);
  if (!RegNo)
    return ParseStatus::NoMatch;

  EndLoc = Parser.getTok().getEndLoc();
  Reg = RegNo;
  Parser.Lex(); // eat register token
  return ParseStatus::Success;
}

//===----------------------------------------------------------------------===//
// Operand parsing
//===----------------------------------------------------------------------===//

bool PenumbraAsmParser::parseOperand(OperandVector &Operands) {
  SMLoc S = Parser.getTok().getLoc();

  // Try register first.
  MCRegister Reg;
  SMLoc RegStart, RegEnd;
  if (tryParseRegister(Reg, RegStart, RegEnd).isSuccess()) {
    Operands.push_back(PenumbraOperand::createReg(Reg, RegStart, RegEnd));
    return false;
  }

  // Punctuation tokens: [ ] +
  if (Parser.getTok().is(AsmToken::LBrac) ||
      Parser.getTok().is(AsmToken::RBrac) ||
      Parser.getTok().is(AsmToken::Plus)) {
    StringRef Tok = Parser.getTok().getString();
    Operands.push_back(PenumbraOperand::createToken(Tok, S));
    Parser.Lex();
    return false;
  }

  // Consume optional '#' prefix on immediates (ARM-style).
  if (Parser.getTok().is(AsmToken::Hash))
    Parser.Lex();

  // Otherwise, try immediate / expression.
  const MCExpr *Expr;
  if (!Parser.parseExpression(Expr)) {
    SMLoc E = Parser.getTok().getLoc();
    Operands.push_back(PenumbraOperand::createImm(Expr, S, E));
    return false;
  }

  return Error(S, "unknown operand");
}

//===----------------------------------------------------------------------===//
// Instruction parsing
//===----------------------------------------------------------------------===//

bool PenumbraAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                         StringRef Name, SMLoc NameLoc,
                                         OperandVector &Operands) {
  // The mnemonic is the first operand (as a token).
  Operands.push_back(PenumbraOperand::createToken(Name, NameLoc));

  // Parse operands.  Commas separate top-level operands but are optional
  // inside memory brackets: "ldw r2, [r1 + #4]" has tokens [ r1 + #4 ]
  // without commas between them.
  while (getLexer().isNot(AsmToken::EndOfStatement)) {
    if (getLexer().is(AsmToken::Comma))
      Parser.Lex(); // eat optional comma
    if (getLexer().is(AsmToken::EndOfStatement))
      break;
    if (parseOperand(Operands))
      return true;
  }

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return Error(getLexer().getLoc(), "unexpected token in operand list");

  Parser.Lex(); // eat EndOfStatement
  return false;
}

//===----------------------------------------------------------------------===//
// Instruction matching
//===----------------------------------------------------------------------===//

bool PenumbraAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                                OperandVector &Operands,
                                                MCStreamer &Out,
                                                uint64_t &ErrorInfo,
                                                bool MatchingInlineAsm) {
  // Intercept pseudo-instructions before TableGen matching.
  StringRef Mnemonic =
      static_cast<PenumbraOperand &>(*Operands[0]).getToken();
  if (Mnemonic.equals_insensitive("li"))
    return emitLoadImmediate(IDLoc, Operands, Out);
  if (Mnemonic.equals_insensitive("la"))
    return emitLoadAddress(IDLoc, Operands, Out);
  if (Mnemonic.equals_insensitive("nop")) {
    // NOP → ADD R0, R0 (encoding 0x00000000)
    MCInst Inst;
    Inst.setOpcode(Penumbra::ADD);
    Inst.addOperand(MCOperand::createReg(Penumbra::R0));
    Inst.addOperand(MCOperand::createReg(Penumbra::R0));
    Inst.addOperand(MCOperand::createReg(Penumbra::R0));
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  }
  if (Mnemonic.equals_insensitive("ret")) {
    // RET → JMP R13
    MCInst Inst;
    Inst.setOpcode(Penumbra::JMP);
    Inst.addOperand(MCOperand::createReg(Penumbra::R13));
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  }

  MCInst Inst;
  switch (MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm)) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL && ErrorInfo < Operands.size())
      ErrorLoc = Operands[ErrorInfo]->getStartLoc();
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires a feature not currently enabled");
  default:
    return Error(IDLoc, "unable to match instruction");
  }
}

//===----------------------------------------------------------------------===//
// Pseudo-instruction expansion
//===----------------------------------------------------------------------===//

void PenumbraAsmParser::emitLLI_LUI_Pair(MCRegister Rd, const MCExpr *Expr,
                                          SMLoc Loc, MCStreamer &Out) {
  // LLI Rd, %lo16(Expr)
  const MCExpr *Lo =
      MCSpecifierExpr::create(Expr, Penumbra::S_Lo16, getContext(), Loc);
  MCInst LLIInst;
  LLIInst.setOpcode(Penumbra::LLI);
  LLIInst.addOperand(MCOperand::createReg(Rd));
  LLIInst.addOperand(MCOperand::createExpr(Lo));
  LLIInst.setLoc(Loc);
  Out.emitInstruction(LLIInst, getSTI());

  // LUI Rd, Rd, %hi16(Expr)
  const MCExpr *Hi =
      MCSpecifierExpr::create(Expr, Penumbra::S_Hi16, getContext(), Loc);
  MCInst LUIInst;
  LUIInst.setOpcode(Penumbra::LUI);
  LUIInst.addOperand(MCOperand::createReg(Rd));
  LUIInst.addOperand(MCOperand::createReg(Rd));
  LUIInst.addOperand(MCOperand::createExpr(Hi));
  LUIInst.setLoc(Loc);
  Out.emitInstruction(LUIInst, getSTI());
}

bool PenumbraAsmParser::emitLoadImmediate(SMLoc IDLoc,
                                           OperandVector &Operands,
                                           MCStreamer &Out) {
  // Syntax: LI Rd, #imm  (or LI Rd, symbol)
  if (Operands.size() != 3)
    return Error(IDLoc, "li requires a register and an immediate operand");

  auto &RdOp = static_cast<PenumbraOperand &>(*Operands[1]);
  auto &ImmOp = static_cast<PenumbraOperand &>(*Operands[2]);

  if (!RdOp.isReg())
    return Error(RdOp.getStartLoc(), "expected register");
  if (!ImmOp.isImm())
    return Error(ImmOp.getStartLoc(), "expected immediate or symbol");

  MCRegister Rd = RdOp.getReg();
  const MCExpr *Expr = ImmOp.getImmVal();

  // Try to evaluate as a constant.
  int64_t Val;
  if (Expr->evaluateAsAbsolute(Val)) {
    if (Val >= 0 && Val <= 0xFFFF) {
      // Small non-negative: single LLI.
      MCInst Inst;
      Inst.setOpcode(Penumbra::LLI);
      Inst.addOperand(MCOperand::createReg(Rd));
      Inst.addOperand(MCOperand::createImm(Val));
      Inst.setLoc(IDLoc);
      Out.emitInstruction(Inst, getSTI());
      return false;
    }
    if (Val >= -32768 && Val < 0) {
      // Small negative: single LLIS.
      MCInst Inst;
      Inst.setOpcode(Penumbra::LLIS);
      Inst.addOperand(MCOperand::createReg(Rd));
      Inst.addOperand(MCOperand::createImm(Val));
      Inst.setLoc(IDLoc);
      Out.emitInstruction(Inst, getSTI());
      return false;
    }
    // Full 32-bit constant: LLI low + LUI high.
    MCInst LLIInst;
    LLIInst.setOpcode(Penumbra::LLI);
    LLIInst.addOperand(MCOperand::createReg(Rd));
    LLIInst.addOperand(MCOperand::createImm(Val & 0xFFFF));
    LLIInst.setLoc(IDLoc);
    Out.emitInstruction(LLIInst, getSTI());

    MCInst LUIInst;
    LUIInst.setOpcode(Penumbra::LUI);
    LUIInst.addOperand(MCOperand::createReg(Rd));
    LUIInst.addOperand(MCOperand::createReg(Rd));
    LUIInst.addOperand(MCOperand::createImm((Val >> 16) & 0xFFFF));
    LUIInst.setLoc(IDLoc);
    Out.emitInstruction(LUIInst, getSTI());
    return false;
  }

  // Symbolic expression: emit LLI+LUI with lo16/hi16 relocations.
  emitLLI_LUI_Pair(Rd, Expr, IDLoc, Out);
  return false;
}

bool PenumbraAsmParser::emitLoadAddress(SMLoc IDLoc, OperandVector &Operands,
                                         MCStreamer &Out) {
  // Syntax: LA Rd, symbol  (or LA Rd, #expr)
  if (Operands.size() != 3)
    return Error(IDLoc, "la requires a register and a symbol operand");

  auto &RdOp = static_cast<PenumbraOperand &>(*Operands[1]);
  auto &SymOp = static_cast<PenumbraOperand &>(*Operands[2]);

  if (!RdOp.isReg())
    return Error(RdOp.getStartLoc(), "expected register");
  if (!SymOp.isImm())
    return Error(SymOp.getStartLoc(), "expected symbol or expression");

  MCRegister Rd = RdOp.getReg();
  const MCExpr *Expr = SymOp.getImmVal();

  // Try constant — LA with a numeric address is equivalent to LI.
  int64_t Val;
  if (Expr->evaluateAsAbsolute(Val)) {
    // Reuse the LI expansion path.
    return emitLoadImmediate(IDLoc, Operands, Out);
  }

  // Symbolic: emit LLI+LUI with lo16/hi16 relocations.
  emitLLI_LUI_Pair(Rd, Expr, IDLoc, Out);
  return false;
}

unsigned
PenumbraAsmParser::validateTargetOperandClass(MCParsedAsmOperand &Op,
                                              unsigned Kind) {
  return Match_InvalidOperand;
}

//===----------------------------------------------------------------------===//
// Auto-generated matcher implementation
//===----------------------------------------------------------------------===//

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "PenumbraGenAsmMatcher.inc"

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraAsmParser() {
  RegisterMCAsmParser<PenumbraAsmParser> X(getThePenumbraTarget());
}

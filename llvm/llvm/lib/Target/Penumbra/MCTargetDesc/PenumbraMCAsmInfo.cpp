//===-- PenumbraMCAsmInfo.cpp - Penumbra Asm Properties -------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCAsmInfo.h"
#include "PenumbraFixupKinds.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

PenumbraMCAsmInfo::PenumbraMCAsmInfo(const Triple &TT) {
  // Penumbra is little-endian, 32-bit
  IsLittleEndian = true;
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;

  // Assembly syntax — ";" is the default statement separator (SeparatorString),
  // which NetBSD asm.h macros depend on for multi-statement macro expansions.
  // Use "//" for comments (like ARM/AArch64) to avoid the conflict.
  CommentString = "//";
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;

  // Data directives
  Data32bitsDirective = "\t.word\t";
  Data16bitsDirective = "\t.half\t";
  Data8bitsDirective = "\t.byte\t";
  ZeroDirective = "\t.zero\t";
}

void PenumbraMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                           const MCSpecifierExpr &Expr) const {
  switch (Expr.getSpecifier()) {
  case Penumbra::S_Lo16:
    OS << "%lo16(";
    break;
  case Penumbra::S_Hi16:
    OS << "%hi16(";
    break;
  case Penumbra::S_PCRel:
    OS << "%pcrel(";
    break;
  case Penumbra::S_TLSgd_Lo16:
    OS << "%tlsgd_lo16(";
    break;
  case Penumbra::S_TLSgd_Hi16:
    OS << "%tlsgd_hi16(";
    break;
  case Penumbra::S_TLSgd_PCRel:
    OS << "%tlsgd_pcrel(";
    break;
  case Penumbra::S_GOT_PCRel_Lo16:
    OS << "%got_pcrel_lo16(";
    break;
  case Penumbra::S_GOT_PCRel_Hi16:
    OS << "%got_pcrel_hi16(";
    break;
  case Penumbra::S_TLSgd_GOT_PCRel_Lo16:
    OS << "%tlsgd_got_pcrel_lo16(";
    break;
  case Penumbra::S_TLSgd_GOT_PCRel_Hi16:
    OS << "%tlsgd_got_pcrel_hi16(";
    break;
  case Penumbra::S_PCRel_Lo16:
    OS << "%pcrel_lo16(";
    break;
  case Penumbra::S_PCRel_Hi16:
    OS << "%pcrel_hi16(";
    break;
  default:
    OS << "%unknown(";
    break;
  }
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}

bool PenumbraMCAsmInfo::evaluateAsRelocatableImpl(const MCSpecifierExpr &Expr,
                                                  MCValue &Res,
                                                  const MCAssembler *Asm) const {
  // PC-anchored specifiers take a label-difference operand naming the
  // sequence's anchor instruction: %got_pcrel_lo16(sym - .LPC0).  The
  // subtracted label is local and same-section, so the ELF writer folds
  // it into the addend (A = P - Q per the PC-anchored relocation pair
  // convention in doc/system/abi.md).  All other specifiers keep the
  // generic rule: no subtracted symbol under a specifier.
  bool AllowSubSym;
  const MCAssembler *EvalAsm = Asm;
  switch (Expr.getSpecifier()) {
  case Penumbra::S_GOT_PCRel_Lo16:
  case Penumbra::S_GOT_PCRel_Hi16:
  case Penumbra::S_TLSgd_PCRel:
  case Penumbra::S_TLSgd_GOT_PCRel_Lo16:
  case Penumbra::S_TLSgd_GOT_PCRel_Hi16:
    // GOT-indirect operands must reach the object writer as symbol
    // references even when the target symbol is defined in the same
    // section (e.g. a static function) — the GOT slot is a link-time
    // entity.  Evaluate without layout so the anchor difference is
    // never folded to a constant.
    EvalAsm = nullptr;
    AllowSubSym = true;
    break;
  case Penumbra::S_PCRel:
  case Penumbra::S_PCRel_Lo16:
  case Penumbra::S_PCRel_Hi16:
    // Anchor differences against same-section symbols (jump table and
    // block-address bases, and PC-relative-direct globals) may fold to an
    // assembly-time constant — that is the resolved no-relocation case.
    // Unlike the GOT specifiers, EvalAsm is kept so same-section symbols
    // resolve in place; cross-section references emit a relocation.
    AllowSubSym = true;
    break;
  default:
    AllowSubSym = false;
    break;
  }

  if (!Expr.getSubExpr()->evaluateAsRelocatable(Res, EvalAsm))
    return false;
  Res.setSpecifier(Expr.getSpecifier());
  return AllowSubSym || !Res.getSubSym();
}

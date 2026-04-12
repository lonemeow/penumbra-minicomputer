//===-- PenumbraMCAsmInfo.cpp - Penumbra Asm Properties -------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCAsmInfo.h"
#include "PenumbraFixupKinds.h"
#include "llvm/MC/MCExpr.h"
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
  default:
    OS << "%unknown(";
    break;
  }
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}

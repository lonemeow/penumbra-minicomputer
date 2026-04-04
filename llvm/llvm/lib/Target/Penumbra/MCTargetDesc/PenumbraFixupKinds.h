//===-- PenumbraFixupKinds.h - Penumbra Fixup Entries -----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAFIXUPKINDS_H
#define LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm::Penumbra {

enum Fixups {
  // 22-bit PC-relative branch offset (Format B, bits [25:4]).
  fixup_penumbra_branch22 = FirstTargetFixupKind,
  // 16-bit immediate (Format L imm16, bits [15:0]).
  fixup_penumbra_imm16,
  // 16-bit memory offset (Format M, bits [17:2]).
  fixup_penumbra_memoffset16,
  // Low 16 bits of a 32-bit absolute address (for LI/LA expansion).
  fixup_penumbra_lo16,
  // High 16 bits of a 32-bit absolute address (for LI/LA expansion).
  fixup_penumbra_hi16,
  // 16-bit PC-relative memory offset (Format M, bits [17:2]).
  // Used for LDW Rd, [PC + %pcrel(sym)] in PIC code.
  fixup_penumbra_memoffset16_pcrel,
  // 16-bit PC-relative immediate (Format L, bits [15:0]).
  // Used for ADDi Rd, %pcrel(sym) in PIC address materialization.
  fixup_penumbra_imm16_pcrel,

  // Marker
  NumTargetFixupKinds
};

// MCSpecifierExpr specifier values for expression modifiers.
// Used by the AsmParser and instruction selector for symbolic operands.
enum Specifier {
  S_None = 0,
  S_Lo16,
  S_Hi16,
  S_PCRel,  // %pcrel() — PC-relative offset
};

} // namespace llvm::Penumbra

#endif // LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAFIXUPKINDS_H

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

  // Marker
  NumTargetFixupKinds
};

// MCSpecifierExpr specifier values for lo16/hi16 expression modifiers.
// Used by the AsmParser when expanding LI/LA pseudo-instructions with
// symbolic operands.
enum Specifier {
  S_None = 0,
  S_Lo16,
  S_Hi16,
};

} // namespace llvm::Penumbra

#endif // LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAFIXUPKINDS_H

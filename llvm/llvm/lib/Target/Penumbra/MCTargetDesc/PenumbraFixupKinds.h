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
  // TLS General-Dynamic: low 16 bits of GD argument (TP-relative offset for
  // static linking, GOT entry address for dynamic linking), into bits [15:0].
  fixup_penumbra_tls_gd_lo16,
  // TLS General-Dynamic: high 16 bits of GD argument, into bits [15:0].
  fixup_penumbra_tls_gd_hi16,
  // TLS General-Dynamic: PC-relative offset to GOT tls_index entry (PIC).
  // Format L, bits [15:0]. Used in ADDi for PIC TLS access.
  fixup_penumbra_tls_gd_pcrel,
  // GOT PC-relative: low 16 bits of (GOT[sym] - P + A).
  // Used in LLI for PIC global address materialization via GOT.
  fixup_penumbra_got_pcrel_lo16,
  // GOT PC-relative: high 16 bits of (GOT[sym] - P + A).
  // Used in LUI for PIC global address materialization via GOT.
  fixup_penumbra_got_pcrel_hi16,
  // TLS GD GOT PC-relative: low 16 bits of (GOT_tls_pair - P + A).
  // Used in LLI for PIC TLS General-Dynamic access via GOT.
  fixup_penumbra_tls_gd_got_pcrel_lo16,
  // TLS GD GOT PC-relative: high 16 bits of (GOT_tls_pair - P + A).
  // Used in LUI for PIC TLS General-Dynamic access via GOT.
  fixup_penumbra_tls_gd_got_pcrel_hi16,

  // Marker
  NumTargetFixupKinds
};

// MCSpecifierExpr specifier values for expression modifiers.
// Used by the AsmParser and instruction selector for symbolic operands.
enum Specifier {
  S_None = 0,
  S_Lo16,
  S_Hi16,
  S_PCRel,     // %pcrel() — PC-relative offset
  S_TLSgd_Lo16,  // %tlsgd_lo16() — TLS General-Dynamic, low 16 bits
  S_TLSgd_Hi16,  // %tlsgd_hi16() — TLS General-Dynamic, high 16 bits
  S_TLSgd_PCRel, // %tlsgd_pcrel() — TLS GD PC-relative (PIC)
  S_GOT_PCRel_Lo16, // %got_pcrel_lo16() — GOT entry, PC-relative, low 16
  S_GOT_PCRel_Hi16, // %got_pcrel_hi16() — GOT entry, PC-relative, high 16
  S_TLSgd_GOT_PCRel_Lo16, // %tlsgd_got_pcrel_lo16() — TLS GD GOT, PC-rel, lo16
  S_TLSgd_GOT_PCRel_Hi16, // %tlsgd_got_pcrel_hi16() — TLS GD GOT, PC-rel, hi16
};

} // namespace llvm::Penumbra

#endif // LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAFIXUPKINDS_H

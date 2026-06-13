//===-- PenumbraMCCodeEmitter.cpp - Penumbra MCInst to bytes --------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFixupKinds.h"
#include "PenumbraMCTargetDesc.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/Casting.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>

using namespace llvm;

namespace {

class PenumbraMCCodeEmitter : public MCCodeEmitter {
  const MCInstrInfo &MCII;
  MCContext &Ctx;

public:
  PenumbraMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx)
      : MCII(MCII), Ctx(Ctx) {}

  void encodeInstruction(const MCInst &Inst, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  // TableGen-generated: returns the binary encoding for an instruction.
  uint64_t getBinaryCodeForInstr(const MCInst &Inst,
                                 SmallVectorImpl<MCFixup> &Fixups,
                                 const MCSubtargetInfo &STI) const;

  unsigned getMachineOpValue(const MCInst &Inst, const MCOperand &MO,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;

  /// Encode a 22-bit PC-relative branch target.  For resolved immediates,
  /// returns the value directly.  For symbolic expressions (labels), creates
  /// a fixup_penumbra_branch22 fixup and returns 0.
  unsigned encodeBranchTarget(const MCInst &Inst, unsigned OpNo,
                              SmallVectorImpl<MCFixup> &Fixups,
                              const MCSubtargetInfo &STI) const;

  /// Encode a 16-bit immediate that may be a symbolic expression.
  unsigned encodeImm16(const MCInst &Inst, unsigned OpNo,
                       SmallVectorImpl<MCFixup> &Fixups,
                       const MCSubtargetInfo &STI) const;

  /// Encode a 16-bit memory offset (Format M, bits [17:2]).
  unsigned encodeMemOffset16(const MCInst &Inst, unsigned OpNo,
                             SmallVectorImpl<MCFixup> &Fixups,
                             const MCSubtargetInfo &STI) const;
};

} // anonymous namespace

void PenumbraMCCodeEmitter::encodeInstruction(
    const MCInst &Inst, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  unsigned Value = getBinaryCodeForInstr(Inst, Fixups, STI);
  support::endian::write<uint32_t>(CB, Value, llvm::endianness::little);
}

// Range-check a literal immediate and error via MCContext if out of range.
// Treats the valid range as the union of signed and unsigned for the given
// bit width: for 16 bits, accept [-32768, 65535]; for N bits generally,
// accept [-(1<<(N-1)), (1<<N) - 1].  That's wide enough to cover every
// instruction that uses the field (signed LLIS and unsigned LLI both fit),
// but still catches silent truncation of values that don't fit in N bits.
static void checkImmRange(MCContext &Ctx, const MCInst &Inst, int64_t Value,
                          unsigned Bits, const char *Field) {
  int64_t Min = -(int64_t(1) << (Bits - 1));
  int64_t Max = (int64_t(1) << Bits) - 1;
  if (Value < Min || Value > Max) {
    SmallString<128> Msg;
    raw_svector_ostream OS(Msg);
    OS << "immediate " << Value << " out of range [" << Min << ", " << Max
       << "] for " << Field;
    Ctx.reportError(Inst.getLoc(), OS.str());
  }
}

// Signed-only range check (for memory offsets, which are sign-extended).
static void checkSimmRange(MCContext &Ctx, const MCInst &Inst, int64_t Value,
                           unsigned Bits, const char *Field) {
  int64_t Min = -(int64_t(1) << (Bits - 1));
  int64_t Max = (int64_t(1) << (Bits - 1)) - 1;
  if (Value < Min || Value > Max) {
    SmallString<128> Msg;
    raw_svector_ostream OS(Msg);
    OS << "immediate " << Value << " out of range [" << Min << ", " << Max
       << "] for " << Field;
    Ctx.reportError(Inst.getLoc(), OS.str());
  }
}

unsigned PenumbraMCCodeEmitter::getMachineOpValue(
    const MCInst &Inst, const MCOperand &MO, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  if (MO.isExpr()) {
    // Try to evaluate constant expressions (e.g. "label - .").
    int64_t Val;
    if (MO.getExpr()->evaluateAsAbsolute(Val))
      return static_cast<unsigned>(Val);
    // Non-constant expression: create an imm16 fixup for the linker.
    Fixups.push_back(MCFixup::create(
        0, MO.getExpr(),
        static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16)));
    return 0;
  }
  llvm_unreachable("Unhandled operand kind in getMachineOpValue");
}

unsigned PenumbraMCCodeEmitter::encodeBranchTarget(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = Inst.getOperand(OpNo);
  if (MO.isImm()) {
    // Format B: 22-bit signed word offset.
    checkSimmRange(Ctx, Inst, MO.getImm(), 22, "branch target (word offset)");
    return static_cast<unsigned>(MO.getImm());
  }

  // Symbolic expression — create a fixup for the linker/relaxer to resolve.
  // Format B: offset22 lives in bits [25:4], fixup applied at byte offset 0.
  Fixups.push_back(MCFixup::create(
      0, MO.getExpr(),
      static_cast<MCFixupKind>(Penumbra::fixup_penumbra_branch22),
      /*PCRel=*/true));
  return 0;
}

unsigned PenumbraMCCodeEmitter::encodeImm16(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = Inst.getOperand(OpNo);
  if (MO.isImm()) {
    checkImmRange(Ctx, Inst, MO.getImm(), 16, "imm16");
    return static_cast<unsigned>(MO.getImm());
  }

  // Check for lo16/hi16/pcrel specifier expressions.
  const MCExpr *Expr = MO.getExpr();
  MCFixupKind Kind =
      static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16);
  bool PCRel = false;

  if (const auto *SE = dyn_cast<MCSpecifierExpr>(Expr)) {
    switch (SE->getSpecifier()) {
    case Penumbra::S_Lo16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_lo16);
      break;
    case Penumbra::S_Hi16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_hi16);
      break;
    case Penumbra::S_TLSgd_Lo16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_lo16);
      break;
    case Penumbra::S_TLSgd_Hi16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_hi16);
      break;
    case Penumbra::S_TLSgd_PCRel:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_pcrel);
      break;
    case Penumbra::S_PCRel: {
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16_pcrel);
      // Anchor-labeled operands (%pcrel(sym - .LPC)) carry their PC
      // reference point explicitly; subtracting P again through a
      // PC-relative fixup would double-count it.  Bare-symbol operands
      // keep the implied anchor-at-P semantics.
      MCValue V;
      PCRel = !(SE->getSubExpr()->evaluateAsRelocatable(V, nullptr) &&
                V.getSubSym());
      break;
    }
    // The GOT-indirect fixups resolve only through their relocation
    // types, which are PC-relative by definition (S + A - P); the
    // assembler never resolves them in place, so the fixup itself is
    // not marked PC-relative.  This also lets the writer fold an
    // anchor-label difference into the addend.
    case Penumbra::S_GOT_PCRel_Lo16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_got_pcrel_lo16);
      break;
    case Penumbra::S_GOT_PCRel_Hi16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_got_pcrel_hi16);
      break;
    case Penumbra::S_TLSgd_GOT_PCRel_Lo16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_got_pcrel_lo16);
      break;
    case Penumbra::S_TLSgd_GOT_PCRel_Hi16:
      Kind = static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_got_pcrel_hi16);
      break;
    default:
      break;
    }
  }

  // Format L: imm16 lives in bits [15:0], fixup applied at byte offset 0.
  Fixups.push_back(MCFixup::create(0, Expr, Kind, PCRel));
  return 0;
}

unsigned PenumbraMCCodeEmitter::encodeMemOffset16(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = Inst.getOperand(OpNo);
  if (MO.isImm()) {
    // Format M: 16-bit signed memory offset.
    checkSimmRange(Ctx, Inst, MO.getImm(), 16, "memory offset");
    return static_cast<unsigned>(MO.getImm());
  }

  // Format M: offset16 lives in bits [17:2].
  // Check for %pcrel() specifier → PC-relative fixup.
  const MCExpr *Expr = MO.getExpr();
  MCFixupKind Kind =
      static_cast<MCFixupKind>(Penumbra::fixup_penumbra_memoffset16);
  bool PCRel = false;

  if (const auto *SE = dyn_cast<MCSpecifierExpr>(Expr)) {
    if (SE->getSpecifier() == Penumbra::S_PCRel) {
      Kind = static_cast<MCFixupKind>(
          Penumbra::fixup_penumbra_memoffset16_pcrel);
      PCRel = true;
    }
  }

  Fixups.push_back(MCFixup::create(0, Expr, Kind, PCRel));
  return 0;
}

#include "PenumbraGenMCCodeEmitter.inc"

MCCodeEmitter *llvm::createPenumbraMCCodeEmitter(const MCInstrInfo &MCII,
                                                 MCContext &Ctx) {
  return new PenumbraMCCodeEmitter(MCII, Ctx);
}

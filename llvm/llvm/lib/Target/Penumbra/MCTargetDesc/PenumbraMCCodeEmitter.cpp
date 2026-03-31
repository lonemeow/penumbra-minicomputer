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
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
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
};

} // anonymous namespace

void PenumbraMCCodeEmitter::encodeInstruction(
    const MCInst &Inst, SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups, const MCSubtargetInfo &STI) const {
  unsigned Value = getBinaryCodeForInstr(Inst, Fixups, STI);
  support::endian::write<uint32_t>(CB, Value, llvm::endianness::little);
}

unsigned PenumbraMCCodeEmitter::getMachineOpValue(
    const MCInst &Inst, const MCOperand &MO, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  if (MO.isReg())
    return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg());
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());
  llvm_unreachable("Unhandled operand kind in getMachineOpValue");
}

unsigned PenumbraMCCodeEmitter::encodeBranchTarget(
    const MCInst &Inst, unsigned OpNo, SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  const MCOperand &MO = Inst.getOperand(OpNo);
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());

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
  if (MO.isImm())
    return static_cast<unsigned>(MO.getImm());

  // Symbolic expression — create a fixup for 16-bit immediate.
  // Format L: imm16 lives in bits [15:0], fixup applied at byte offset 0.
  Fixups.push_back(MCFixup::create(
      0, MO.getExpr(),
      static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16)));
  return 0;
}

#include "PenumbraGenMCCodeEmitter.inc"

MCCodeEmitter *llvm::createPenumbraMCCodeEmitter(const MCInstrInfo &MCII,
                                                 MCContext &Ctx) {
  return new PenumbraMCCodeEmitter(MCII, Ctx);
}

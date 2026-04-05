//===-- PenumbraDisassembler.h - Disassembler for Penumbra ------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_DISASSEMBLER_PENUMBRADISASSEMBLER_H
#define LLVM_LIB_TARGET_PENUMBRA_DISASSEMBLER_PENUMBRADISASSEMBLER_H

#include "llvm/MC/MCDisassembler/MCDisassembler.h"

namespace llvm {

class PenumbraDisassembler : public MCDisassembler {
public:
  PenumbraDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx)
      : MCDisassembler(STI, Ctx) {}

  MCDisassembler::DecodeStatus
  getInstruction(MCInst &Instr, uint64_t &Size, ArrayRef<uint8_t> Bytes,
                 uint64_t Address, raw_ostream &CStream) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_DISASSEMBLER_PENUMBRADISASSEMBLER_H

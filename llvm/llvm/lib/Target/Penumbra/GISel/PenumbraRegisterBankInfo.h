//===-- PenumbraRegisterBankInfo.h ------------------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAREGISTERBANKINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAREGISTERBANKINFO_H

#include "llvm/CodeGen/RegisterBankInfo.h"

#define GET_REGBANK_DECLARATIONS
#include "PenumbraGenRegisterBank.inc"
#undef GET_REGBANK_DECLARATIONS

namespace llvm {

class TargetRegisterInfo;

class PenumbraGenRegisterBankInfo : public RegisterBankInfo {
protected:
#define GET_TARGET_REGBANK_CLASS
#include "PenumbraGenRegisterBank.inc"
#undef GET_TARGET_REGBANK_CLASS
};

class PenumbraRegisterBankInfo final : public PenumbraGenRegisterBankInfo {
public:
  PenumbraRegisterBankInfo(const TargetRegisterInfo &TRI);

  const InstructionMapping &
  getInstrMapping(const MachineInstr &MI) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAREGISTERBANKINFO_H

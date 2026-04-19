//===-- PenumbraRegisterInfo.h - Penumbra Register Info ----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAREGISTERINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "PenumbraGenRegisterInfo.inc"

namespace llvm {

class PenumbraRegisterInfo : public PenumbraGenRegisterInfo {
public:
  PenumbraRegisterInfo();

  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;
  const uint32_t *getCallPreservedMask(const MachineFunction &MF,
                                       CallingConv::ID CC) const override;

  BitVector getReservedRegs(const MachineFunction &MF) const override;

  bool eliminateFrameIndex(MachineBasicBlock::iterator MI, int SPAdj,
                           unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;

  // Large frame offsets need a scratch register to materialize, and the
  // elimination runs after register allocation — so we ask PEI to keep
  // a RegScavenger alive across the pass so we can grab a free register
  // at the rewrite point.
  bool requiresRegisterScavenging(const MachineFunction &MF) const override {
    return true;
  }
  bool requiresFrameIndexScavenging(const MachineFunction &MF) const override {
    return true;
  }

  Register getFrameRegister(const MachineFunction &MF) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAREGISTERINFO_H

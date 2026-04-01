//===-- PenumbraFrameLowering.h - Penumbra Frame Lowering --------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAFRAMELOWERING_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAFRAMELOWERING_H

#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class PenumbraFrameLowering : public TargetFrameLowering {
public:
  PenumbraFrameLowering()
      : TargetFrameLowering(StackGrowsDown, /*StackAlign=*/Align(4),
                            /*LocalAreaOffset=*/0) {}

  void emitPrologue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF,
                    MachineBasicBlock &MBB) const override;
  bool hasFPImpl(const MachineFunction &MF) const override { return false; }

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAFRAMELOWERING_H

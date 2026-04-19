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
  bool hasFPImpl(const MachineFunction &MF) const override;

  // Called before PEI finalizes frame offsets.  Creates an emergency
  // spill slot so that RegScavenger can always satisfy a
  // scavengeRegisterBackwards request in eliminateFrameIndex — even in
  // register-pressure-heavy functions where no register happens to be
  // free at the rewrite point.
  void processFunctionBeforeFrameFinalized(MachineFunction &MF,
                                           RegScavenger *RS) const override;

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS) const override;

  bool
  spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MI,
                            ArrayRef<CalleeSavedInfo> CSI,
                            const TargetRegisterInfo *TRI) const override;

  bool
  restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MI,
                              MutableArrayRef<CalleeSavedInfo> CSI,
                              const TargetRegisterInfo *TRI) const override;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator MI) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAFRAMELOWERING_H

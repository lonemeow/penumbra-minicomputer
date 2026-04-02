//===-- PenumbraMachineFunctionInfo.h - Penumbra MFI -----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Per-function machine-level state for the Penumbra backend.
// Currently holds the frame index of the variadic argument save area.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

class PenumbraMachineFunctionInfo : public MachineFunctionInfo {
  /// Frame index of the register save area for variadic functions.
  /// Set during lowerFormalArguments when F.isVarArg() is true.
  int VarArgsFrameIndex = 0;

public:
  PenumbraMachineFunctionInfo(const Function &F,
                              const TargetSubtargetInfo *STI) {}

  int getVarArgsFrameIndex() const { return VarArgsFrameIndex; }
  void setVarArgsFrameIndex(int FI) { VarArgsFrameIndex = FI; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAMACHINEFUNCTIONINFO_H

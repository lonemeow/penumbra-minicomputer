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
#include "llvm/CodeGen/Register.h"

namespace llvm {

class PenumbraMachineFunctionInfo : public MachineFunctionInfo {
  /// Frame index of the register save area for variadic functions.
  /// Set during lowerFormalArguments when F.isVarArg() is true.
  int VarArgsFrameIndex = 0;

  /// Virtual register capturing R13 (LR) at function entry.  Used by
  /// __builtin_return_address(0) to read the *original* caller return
  /// address rather than the live value of R13, which every BL/JALR
  /// clobbers with its own call-site return address.  Lazily created
  /// by the instruction selector on the first intrinsic reference;
  /// reused for any subsequent references in the same function.
  Register ReturnAddressVReg;

  /// Next unique id for PC-anchor labels (.LPC<function>_<id>).  Each
  /// PC-anchored address sequence allocates one id at selection time to
  /// pair its immediate-carrying instructions with its anchor; the
  /// AsmPrinter materializes the label when lowering the pseudos.
  unsigned PICLabelUId = 0;

public:
  PenumbraMachineFunctionInfo(const Function &F,
                              const TargetSubtargetInfo *STI) {}

  int getVarArgsFrameIndex() const { return VarArgsFrameIndex; }
  void setVarArgsFrameIndex(int FI) { VarArgsFrameIndex = FI; }

  Register getReturnAddressVReg() const { return ReturnAddressVReg; }
  void setReturnAddressVReg(Register R) { ReturnAddressVReg = R; }

  unsigned createPICLabelUId() { return PICLabelUId++; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAMACHINEFUNCTIONINFO_H

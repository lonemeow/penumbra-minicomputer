//===-- PenumbraTargetMachine.h - Penumbra Target Machine --------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H

#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"

namespace llvm {

class PenumbraTargetMachine : public CodeGenTargetMachineImpl {
  PenumbraSubtarget Subtarget;

public:
  PenumbraTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                        StringRef FS, const TargetOptions &Options,
                        std::optional<Reloc::Model> RM,
                        std::optional<CodeModel::Model> CM,
                        CodeGenOptLevel OL, bool JIT);

  const PenumbraSubtarget *getSubtargetImpl(const Function &) const override {
    return &Subtarget;
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H

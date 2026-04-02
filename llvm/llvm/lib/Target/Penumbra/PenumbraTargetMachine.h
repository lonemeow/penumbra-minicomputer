//===-- PenumbraTargetMachine.h - Penumbra Target Machine --------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H

#include "PenumbraMachineFunctionInfo.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"

namespace llvm {

class PenumbraTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
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

  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override {
    return PenumbraMachineFunctionInfo::create<PenumbraMachineFunctionInfo>(
        Allocator, F, STI);
  }

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H

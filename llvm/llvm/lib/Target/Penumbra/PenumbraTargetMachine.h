//===-- PenumbraTargetMachine.h - Penumbra Target Machine --------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETMACHINE_H

#include "PenumbraMachineFunctionInfo.h"
#include "PenumbraSubtarget.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"

namespace llvm {

class PenumbraTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;

  // One subtarget per distinct (cpu, tune, features) combination appearing in
  // the module.  Tuning is a per-function property — clang records -mtune on
  // each function it emits — so a single module-wide subtarget could not
  // answer for all of them.
  mutable StringMap<std::unique_ptr<PenumbraSubtarget>> SubtargetMap;

public:
  PenumbraTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                        StringRef FS, const TargetOptions &Options,
                        std::optional<Reloc::Model> RM,
                        std::optional<CodeModel::Model> CM,
                        CodeGenOptLevel OL, bool JIT);

  const PenumbraSubtarget *getSubtargetImpl(const Function &F) const override;

  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetTransformInfo getTargetTransformInfo(const Function &F) const override;

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

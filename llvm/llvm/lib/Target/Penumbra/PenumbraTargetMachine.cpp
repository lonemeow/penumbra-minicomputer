//===-- PenumbraTargetMachine.cpp - Penumbra Target Machine ---------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraTargetMachine.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

static const char *PenumbraDataLayout =
    "e"        // little-endian
    "-m:e"     // ELF mangling
    "-p:32:32" // 32-bit pointers, 32-bit aligned
    "-i32:32"  // i32 is 32-bit aligned
    "-n32"     // native integer width is 32
    "-S32";    // stack is 32-bit aligned

PenumbraTargetMachine::PenumbraTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, PenumbraDataLayout, TT,
                        CPU.empty() ? "penumbra1" : CPU, FS, Options,
                        RM.value_or(Reloc::Static),
                        CM.value_or(CodeModel::Small), OL) {}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTarget() {
  RegisterTargetMachine<PenumbraTargetMachine> X(getThePenumbraTarget());
}

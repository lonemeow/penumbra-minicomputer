//===-- PenumbraTargetInfo.cpp - Penumbra Target Implementation -----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getThePenumbraTarget() {
  static Target ThePenumbraTarget;
  return ThePenumbraTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTargetInfo() {
  RegisterTarget<Triple::penumbra> X(getThePenumbraTarget(), "penumbra",
                                     "Penumbra 32-bit RISC", "Penumbra");
}

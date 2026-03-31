//===-- PenumbraISelLowering.h - Penumbra DAG Lowering Interface --*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class PenumbraSubtarget;

class PenumbraISelLowering : public TargetLowering {
public:
  PenumbraISelLowering(const TargetMachine &TM, const PenumbraSubtarget &STI);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

//===-- PenumbraTargetInfo.h - Penumbra Target Implementation ---*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_TARGETINFO_PENUMBRATARGETINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_TARGETINFO_PENUMBRATARGETINFO_H

namespace llvm {

class Target;

Target &getThePenumbraTarget();

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_TARGETINFO_PENUMBRATARGETINFO_H

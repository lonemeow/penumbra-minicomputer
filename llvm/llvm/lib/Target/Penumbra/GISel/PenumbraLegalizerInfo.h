//===-- PenumbraLegalizerInfo.h ---------------------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRALEGALIZERINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRALEGALIZERINFO_H

#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"

namespace llvm {

class PenumbraSubtarget;

struct PenumbraLegalizerInfo : public LegalizerInfo {
  PenumbraLegalizerInfo(const PenumbraSubtarget &ST);

  bool legalizeCustom(LegalizerHelper &Helper, MachineInstr &MI,
                      LostDebugLocObserver &LocObserver) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRALEGALIZERINFO_H

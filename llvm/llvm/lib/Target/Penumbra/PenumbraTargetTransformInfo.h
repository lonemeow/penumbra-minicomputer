//===- PenumbraTargetTransformInfo.h - Penumbra TTI -------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Penumbra-specific TargetTransformInfo.  We subclass BasicTTIImplBase so
// that target-aware cost-model queries (currently just LSR's
// isLegalAddressingMode) are delegated to PenumbraTargetLowering instead
// of being answered by the generic "RISCy r+r and r+i" defaults.  All
// other queries fall through to the BasicTTIImpl defaults.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETTRANSFORMINFO_H

#include "PenumbraISelLowering.h"
#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/IR/Function.h"

namespace llvm {

class PenumbraTTIImpl final : public BasicTTIImplBase<PenumbraTTIImpl> {
  using BaseT = BasicTTIImplBase<PenumbraTTIImpl>;
  using TTI = TargetTransformInfo;

  friend BaseT;

  const PenumbraSubtarget *ST;
  const PenumbraISelLowering *TLI;

  const PenumbraSubtarget *getST() const { return ST; }
  const PenumbraISelLowering *getTLI() const { return TLI; }

public:
  explicit PenumbraTTIImpl(const PenumbraTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  // Register-class identity for cost-model queries.  We have a single
  // GPR file; FP and vector classes exist conceptually so the framework
  // can ask "do you have any?", but we report zero registers in those
  // classes below to short-circuit FP / vectorization paths that we
  // can't legalize.
  enum PenumbraRegisterClass { GPRRC, FPRRC, VRRC };

  // Route every type to GPR — Penumbra is integer-only, no FP unit, no
  // vector unit; floats and doubles live in GPR pairs (soft-float ABI).
  unsigned getRegisterClassForType(bool Vector,
                                   Type *Ty = nullptr) const {
    if (Vector)
      return VRRC;
    return GPRRC;
  }

  // Penumbra has 16 GPRs, 12 of them allocatable for general use
  // (R0=zero, R12=TP, R14=SP, R15=PC are reserved).  Reporting the real
  // count to LSR avoids the over-conservative default-of-8 that pushed
  // tight pointer-bump loops toward base+index forms.  FP and vector
  // classes return 0 — without this guard, BasicTTIImplBase's defaults
  // would let the loop vectorizer try to vectorize, producing IR types
  // we have no legalization for.
  unsigned getNumberOfRegisters(unsigned ClassID) const override {
    switch (ClassID) {
    case GPRRC:
      return 12;
    case FPRRC:
    case VRRC:
    default:
      return 0;
    }
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETTRANSFORMINFO_H

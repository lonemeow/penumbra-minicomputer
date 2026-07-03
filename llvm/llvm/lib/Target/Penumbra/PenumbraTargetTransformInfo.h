//===- PenumbraTargetTransformInfo.h - Penumbra TTI -------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Penumbra-specific TargetTransformInfo.  This is where IR-level passes
// (LSR, the vectorizers, DivRemPairs, inliner/unroller cost queries) get
// their machine-capability and cost answers; Penumbra has no SelectionDAG
// path, so generic defaults that infer capabilities from SDAG-era state
// answer wrongly and are corrected here.  Queries without an override
// fall through to the BasicTTIImpl defaults.
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

  // DivRemPairs asks whether the target computes quotient and remainder
  // in one operation before deciding what to do with an `x/y; x%y` pair:
  // true keeps the remainder instruction and hoists the pair adjacent —
  // where the GISel pre-legalizer combiner fuses it into a single
  // G_SDIVREM/G_UDIVREM selected to DIV_P/DIVU_P — while false decomposes
  // the remainder into `x - (x/y)*y`.  The TTI base answers false
  // unconditionally; it never consults the SelectionDAG action tables.
  bool hasDivRemOp(Type *DataType, bool IsSigned) const override {
    return DataType->isIntegerTy(32);
  }

  // Make instruction count the primary LSR sort key.  The default
  // `isLSRCostLess` tuple-compares with `NumRegs` first — a register-
  // pressure prior that fits 1990s x86 (8 GPRs, free `[base+index*scale]`
  // folded into the load) but not Penumbra (12 free GPRs, no scaled
  // addressing).  Without this override LSR rewrites tight pointer-bump
  // loops into a single integer IV with `add Rb, Ri` materializing each
  // base+index per iteration — saves one PHI register at the cost of two
  // extra ADDs every iteration.  PowerPC, SystemZ, AArch64, X86, AVR, and
  // Mips all override `isLSRCostLess` for the same reason; we follow the
  // PowerPC pattern directly.
  bool isLSRCostLess(const TargetTransformInfo::LSRCost &C1,
                     const TargetTransformInfo::LSRCost &C2) const override {
    return std::tie(C1.Insns, C1.NumRegs, C1.AddRecCost, C1.NumIVMuls,
                    C1.NumBaseAdds, C1.ScaleCost, C1.ImmCost, C1.SetupCost) <
           std::tie(C2.Insns, C2.NumRegs, C2.AddRecCost, C2.NumIVMuls,
                    C2.NumBaseAdds, C2.ScaleCost, C2.ImmCost, C2.SetupCost);
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRATARGETTRANSFORMINFO_H

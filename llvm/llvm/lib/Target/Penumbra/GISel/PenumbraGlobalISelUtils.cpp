//===-- PenumbraGlobalISelUtils.cpp - Penumbra GISel helpers ----*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraGlobalISelUtils.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

std::optional<std::pair<CmpInst::Predicate, APInt>>
PenumbraGISelUtils::adjacentCompare(CmpInst::Predicate Pred, const APInt &C) {
  if (!ICmpInst::isRelational(Pred))
    return std::nullopt;

  // A step past the end of the predicate's range would wrap, turning a compare
  // that is always false into one that is always true, or the reverse.
  bool Signed = ICmpInst::isSigned(Pred);
  bool StepUp = ICmpInst::isGT(Pred) || ICmpInst::isLE(Pred);
  bool AdjacentWraps;
  if (StepUp)
    AdjacentWraps = Signed ? C.isMaxSignedValue() : C.isMaxValue();
  else
    AdjacentWraps = Signed ? C.isMinSignedValue() : C.isMinValue();
  if (AdjacentWraps)
    return std::nullopt;

  return std::pair(CmpInst::getFlippedStrictnessPredicate(Pred),
                   StepUp ? C + 1 : C - 1);
}

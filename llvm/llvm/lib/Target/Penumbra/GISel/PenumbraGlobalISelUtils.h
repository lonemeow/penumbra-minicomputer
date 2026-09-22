//===-- PenumbraGlobalISelUtils.h - Penumbra GISel helpers ------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Helpers shared by the Penumbra GlobalISel passes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAGLOBALISELUTILS_H
#define LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAGLOBALISELUTILS_H

#include "llvm/ADT/APInt.h"
#include "llvm/IR/InstrTypes.h"
#include <optional>
#include <utility>

namespace llvm {
namespace PenumbraGISelUtils {

// The compare against C+1 or C-1 that means the same as `Pred C` for every
// operand: `x > C` is `x >= C+1` and `x <= C` is `x < C+1`, and `<` and `>=`
// step down.  None for a step that would wrap.
std::optional<std::pair<CmpInst::Predicate, APInt>>
adjacentCompare(CmpInst::Predicate Pred, const APInt &C);

} // namespace PenumbraGISelUtils
} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRAGLOBALISELUTILS_H

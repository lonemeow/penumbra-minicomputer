//===--- Penumbra.cpp - Implement Penumbra target feature support ----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "Penumbra.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

const char *const PenumbraTargetInfo::GCCRegNames[] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

ArrayRef<const char *> PenumbraTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(GCCRegNames);
}

void PenumbraTargetInfo::getTargetDefines(const LangOptions &Opts,
                                          MacroBuilder &Builder) const {
  Builder.defineMacro("__penumbra__");
  Builder.defineMacro("__PENUMBRA__");
}

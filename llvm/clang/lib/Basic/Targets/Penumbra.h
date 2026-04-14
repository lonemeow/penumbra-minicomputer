//===--- Penumbra.h - Declare Penumbra target feature support -----*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_PENUMBRA_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_PENUMBRA_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY PenumbraTargetInfo : public TargetInfo {
  static const char *const GCCRegNames[];

public:
  PenumbraTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    resetDataLayout();
    RegParmMax = 4; // R1-R4 used for argument passing
    // All atomic operations go through __atomic_* libcalls
    // (no inline atomic instructions).  Keep InlineWidth=0 so
    // clang emits libcalls directly (not IR atomics).
    // The LOCK_FREE macros are overridden in getTargetDefines()
    // to report "always lock-free" for <=32-bit types, since
    // the library implementations (RAS userland, interrupt-disable
    // kernel) are always lock-free on uniprocessor.
    MaxAtomicPromoteWidth = 32;
    MaxAtomicInlineWidth = 0;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  ArrayRef<const char *> getGCCRegNames() const override;

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    return {};
  }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::VoidPtrBuiltinVaList;
  }

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &Info) const override {
    return false;
  }

  std::string_view getClobbers() const override { return ""; }

  bool hasBitIntType() const override { return true; }
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_PENUMBRA_H

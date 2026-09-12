//===--- Penumbra.cpp - Implement Penumbra target feature support ----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "Penumbra.h"
#include "clang/Basic/MacroBuilder.h"
#include "llvm/ADT/STLExtras.h"

using namespace clang;
using namespace clang::targets;

// Tuning targets, mirroring the ProcessorModel definitions in
// llvm/lib/Target/Penumbra/Penumbra.td (clang cannot read the backend's
// tables).  All of them accept the same binaries, so any name is valid for
// both -mcpu and -mtune.
static constexpr llvm::StringLiteral ValidCPUNames[] = {
    "generic", "penumbra1", "penumbra2", "penumbra2_5"};

bool PenumbraTargetInfo::isValidCPUName(StringRef Name) const {
  return llvm::is_contained(ValidCPUNames, Name);
}

void PenumbraTargetInfo::fillValidCPUList(
    SmallVectorImpl<StringRef> &Values) const {
  Values.append(std::begin(ValidCPUNames), std::end(ValidCPUNames));
}

bool PenumbraTargetInfo::setCPU(const std::string &Name) {
  if (!isValidCPUName(Name))
    return false;
  CPU = Name;
  return true;
}

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

  // Override LOCK_FREE macros: <=32-bit atomics are always lock-free
  // on uniprocessor Penumbra (RAS userland, interrupt-disable kernel).
  // We keep MaxAtomicInlineWidth=0 so clang emits __atomic_* libcalls
  // (not IR atomics), but the libcalls ARE always lock-free.
  // Without this, libc++ fails to define atomic_signed_lock_free.
  Builder.defineMacro("__GCC_ATOMIC_BOOL_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_CHAR_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_CHAR16_T_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_CHAR32_T_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_SHORT_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_INT_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_LONG_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_POINTER_LOCK_FREE", "2");
  Builder.defineMacro("__GCC_ATOMIC_WCHAR_T_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_BOOL_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_CHAR_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_CHAR16_T_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_CHAR32_T_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_SHORT_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_INT_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_LONG_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_POINTER_LOCK_FREE", "2");
  Builder.defineMacro("__CLANG_ATOMIC_WCHAR_T_LOCK_FREE", "2");
}

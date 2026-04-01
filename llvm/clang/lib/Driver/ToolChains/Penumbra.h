//===--- Penumbra.h - Penumbra ToolChain Implementations ---------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_PENUMBRA_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_PENUMBRA_H

#include "Gnu.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace toolchains {

class LLVM_LIBRARY_VISIBILITY PenumbraToolChain : public Generic_ELF {
public:
  PenumbraToolChain(const Driver &D, const llvm::Triple &Triple,
                    const llvm::opt::ArgList &Args)
      : Generic_ELF(D, Triple, Args) {}

  void addLibCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override {}
  void addLibStdCxxIncludePaths(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override {}
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_PENUMBRA_H

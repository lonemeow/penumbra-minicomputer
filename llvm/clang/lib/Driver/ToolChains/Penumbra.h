//===--- Penumbra.h - Penumbra ToolChain Implementations ---------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_PENUMBRA_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_PENUMBRA_H

#include "Gnu.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Tool.h"

namespace clang {
namespace driver {

namespace tools {
namespace penumbra {

class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("penumbra::Linker", "ld.lld", TC) {}
  bool isLinkJob() const override { return true; }
  bool hasIntegratedCPP() const override { return false; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // namespace penumbra
} // namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY PenumbraToolChain : public Generic_ELF {
public:
  PenumbraToolChain(const Driver &D, const llvm::Triple &Triple,
                    const llvm::opt::ArgList &Args)
      : Generic_ELF(D, Triple, Args) {}

  Tool *buildLinker() const override {
    return new tools::penumbra::Linker(*this);
  }

  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                 llvm::opt::ArgStringList &CC1Args)
      const override;

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

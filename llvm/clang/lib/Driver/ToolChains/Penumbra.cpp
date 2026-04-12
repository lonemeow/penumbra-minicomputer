//===--- Penumbra.cpp - Penumbra ToolChain Implementations -------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "Penumbra.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/InputInfo.h"
#include "llvm/Option/ArgList.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::driver::tools;
using namespace llvm::opt;

/// Bare-metal linker for penumbra-unknown-none.
/// Minimal: no CRT files, no -lc, no -lgcc.  Used for ROM and
/// hardware test programs that supply their own startup code.
void penumbra::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                    const InputInfo &Output,
                                    const InputInfoList &Inputs,
                                    const ArgList &Args,
                                    const char *LinkingOutput) const {
  const ToolChain &TC = getToolChain();
  const Driver &D = TC.getDriver();
  ArgStringList CmdArgs;

  // Emulation string (must match lld's Penumbra target).
  CmdArgs.push_back("-m");
  CmdArgs.push_back("elf32penumbra");

  if (!D.SysRoot.empty())
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

  // Output file.
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Forward all -Wl, flags and linker inputs.
  Args.addAllArgs(CmdArgs, {options::OPT_T_Group, options::OPT_s,
                             options::OPT_t, options::OPT_r});
  Args.addAllArgs(CmdArgs, {options::OPT_L, options::OPT_u});
  TC.AddFilePathLibArgs(Args, CmdArgs);
  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // Forward -shared, -static, -nostdlib, etc.
  Args.addAllArgs(CmdArgs, {options::OPT_shared, options::OPT_static,
                             options::OPT_rdynamic});

  // Self-relocating PIE code (bootloader, benchmarks) reads
  // pre-relocation data values to compute the load bias.
  // Without written addends these are zero under RELA.
  CmdArgs.push_back("--apply-dynamic-relocs");

  // Find ld.lld in the same directory as clang.
  const char *Exec = Args.MakeArgString(TC.GetProgramPath("ld.lld"));
  C.addCommand(std::make_unique<Command>(JA, *this,
                                         ResponseFileSupport::AtFileCurCP(),
                                         Exec, CmdArgs, Inputs, Output));
}

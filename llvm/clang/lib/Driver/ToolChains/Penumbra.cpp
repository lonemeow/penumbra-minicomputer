//===--- Penumbra.cpp - Penumbra ToolChain Implementations -------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "Penumbra.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"

using namespace clang;
using namespace clang::driver;
using namespace clang::driver::tools;
using namespace llvm::opt;

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

  // Match GNU ld behavior: don't error on version script symbols that
  // aren't defined (e.g. Heimdal lists DllMain, a Windows-only symbol).
  CmdArgs.push_back("--undefined-version");

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

  // Add default library search paths and -lc unless suppressed.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    CmdArgs.push_back("-L=/lib");
    CmdArgs.push_back("-L=/usr/lib");
    CmdArgs.push_back("-lc");
  }

  // Find ld.lld in the same directory as clang.
  const char *Exec = Args.MakeArgString(TC.GetProgramPath("ld.lld"));
  C.addCommand(std::make_unique<Command>(JA, *this,
                                         ResponseFileSupport::AtFileCurCP(),
                                         Exec, CmdArgs, Inputs, Output));
}

void toolchains::PenumbraToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Dir(getDriver().ResourceDir);
    llvm::sys::path::append(Dir, "include");
    addSystemInclude(DriverArgs, CC1Args, Dir);
  }

  if (!DriverArgs.hasArg(options::OPT_nostdlibinc)) {
    const std::string &SysRoot = getDriver().SysRoot;
    if (!SysRoot.empty())
      addSystemInclude(DriverArgs, CC1Args, SysRoot + "/usr/include");
  }
}

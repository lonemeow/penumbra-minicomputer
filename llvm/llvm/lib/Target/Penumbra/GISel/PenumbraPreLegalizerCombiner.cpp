//=== PenumbraPreLegalizerCombiner.cpp ------------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Generic MachineIR combines run before the legalizer at -O1 and above.
// Rule set is defined in PenumbraCombine.td.
//
//===----------------------------------------------------------------------===//

#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/CombinerInfo.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetPassConfig.h"

#define GET_GICOMBINER_DEPS
#include "PenumbraGenPreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_DEPS

#define DEBUG_TYPE "penumbra-prelegalizer-combiner"

namespace llvm {
void initializePenumbraPreLegalizerCombinerPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

namespace {

#define GET_GICOMBINER_TYPES
#include "PenumbraGenPreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_TYPES

class PenumbraPreLegalizerCombinerImpl : public Combiner {
protected:
  const CombinerHelper Helper;
  const PenumbraPreLegalizerCombinerImplRuleConfig &RuleConfig;
  const PenumbraSubtarget &STI;

public:
  PenumbraPreLegalizerCombinerImpl(
      MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
      GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
      const PenumbraPreLegalizerCombinerImplRuleConfig &RuleConfig,
      const PenumbraSubtarget &STI, MachineDominatorTree *MDT,
      const LegalizerInfo *LI);

  static const char *getName() { return "PenumbraPreLegalizerCombiner"; }

  bool tryCombineAll(MachineInstr &I) const override;

private:
#define GET_GICOMBINER_CLASS_MEMBERS
#include "PenumbraGenPreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CLASS_MEMBERS
};

#define GET_GICOMBINER_IMPL
#include "PenumbraGenPreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_IMPL

PenumbraPreLegalizerCombinerImpl::PenumbraPreLegalizerCombinerImpl(
    MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
    GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
    const PenumbraPreLegalizerCombinerImplRuleConfig &RuleConfig,
    const PenumbraSubtarget &STI, MachineDominatorTree *MDT,
    const LegalizerInfo *LI)
    : Combiner(MF, CInfo, TPC, &VT, CSEInfo),
      Helper(Observer, B, /*IsPreLegalize*/ true, &VT, MDT, LI),
      RuleConfig(RuleConfig), STI(STI),
#define GET_GICOMBINER_CONSTRUCTOR_INITS
#include "PenumbraGenPreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CONSTRUCTOR_INITS
{
}

class PenumbraPreLegalizerCombiner : public MachineFunctionPass {
public:
  static char ID;

  PenumbraPreLegalizerCombiner();

  StringRef getPassName() const override {
    return "PenumbraPreLegalizerCombiner";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  PenumbraPreLegalizerCombinerImplRuleConfig RuleConfig;
};
} // end anonymous namespace

void PenumbraPreLegalizerCombiner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.setPreservesCFG();
  getSelectionDAGFallbackAnalysisUsage(AU);
  AU.addRequired<GISelValueTrackingAnalysisLegacy>();
  AU.addPreserved<GISelValueTrackingAnalysisLegacy>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addPreserved<MachineDominatorTreeWrapperPass>();
  AU.addRequired<GISelCSEAnalysisWrapperPass>();
  AU.addPreserved<GISelCSEAnalysisWrapperPass>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

PenumbraPreLegalizerCombiner::PenumbraPreLegalizerCombiner()
    : MachineFunctionPass(ID) {
  if (!RuleConfig.parseCommandLineOption())
    report_fatal_error("Invalid rule identifier");
}

bool PenumbraPreLegalizerCombiner::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getProperties().hasFailedISel())
    return false;
  auto &TPC = getAnalysis<TargetPassConfig>();

  GISelCSEAnalysisWrapper &Wrapper =
      getAnalysis<GISelCSEAnalysisWrapperPass>().getCSEWrapper();
  auto *CSEInfo = &Wrapper.get(TPC.getCSEConfig());

  const PenumbraSubtarget &ST = MF.getSubtarget<PenumbraSubtarget>();
  const auto *LI = ST.getLegalizerInfo();

  const Function &F = MF.getFunction();
  bool EnableOpt =
      MF.getTarget().getOptLevel() != CodeGenOptLevel::None && !skipFunction(F);
  GISelValueTracking *VT =
      &getAnalysis<GISelValueTrackingAnalysisLegacy>().get(MF);
  MachineDominatorTree *MDT =
      &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  CombinerInfo CInfo(/*AllowIllegalOps*/ true, /*ShouldLegalizeIllegal*/ false,
                     /*LegalizerInfo*/ nullptr, EnableOpt, F.hasOptSize(),
                     F.hasMinSize());
  // Single-pass, no fixed-point iteration — matches RISC-V's compile-time
  // tradeoff.  The first combiner may see dead instructions from the
  // IRTranslator, so enable full DCE.
  CInfo.MaxIterations = 1;
  CInfo.ObserverLvl = CombinerInfo::ObserverLevel::SinglePass;
  CInfo.EnableFullDCE = true;
  PenumbraPreLegalizerCombinerImpl Impl(MF, CInfo, &TPC, *VT, CSEInfo,
                                        RuleConfig, ST, MDT, LI);
  return Impl.combineMachineInstrs();
}

char PenumbraPreLegalizerCombiner::ID = 0;
INITIALIZE_PASS_BEGIN(PenumbraPreLegalizerCombiner, DEBUG_TYPE,
                      "Combine Penumbra machine instrs before legalization",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(GISelValueTrackingAnalysisLegacy)
INITIALIZE_PASS_DEPENDENCY(GISelCSEAnalysisWrapperPass)
INITIALIZE_PASS_END(PenumbraPreLegalizerCombiner, DEBUG_TYPE,
                    "Combine Penumbra machine instrs before legalization",
                    false, false)

namespace llvm {
FunctionPass *createPenumbraPreLegalizerCombiner() {
  return new PenumbraPreLegalizerCombiner();
}
} // namespace llvm

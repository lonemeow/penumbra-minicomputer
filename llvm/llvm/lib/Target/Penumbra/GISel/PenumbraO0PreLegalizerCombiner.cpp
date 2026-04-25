//=== PenumbraO0PreLegalizerCombiner.cpp ----------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Hand-picked subset of GlobalISel combines that runs at -O0 to clean up
// the most obvious IRTranslator artefacts (DCE, ptr_add chaining, extload
// folding, branch inversion) without paying the compile-time cost of full
// canonicalization.  Rule set is `optnone_combines` — see
// PenumbraCombine.td.
//
//===----------------------------------------------------------------------===//

#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/CombinerInfo.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetPassConfig.h"

#define GET_GICOMBINER_DEPS
#include "PenumbraGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_DEPS

#define DEBUG_TYPE "penumbra-O0-prelegalizer-combiner"

namespace llvm {
void initializePenumbraO0PreLegalizerCombinerPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

namespace {

#define GET_GICOMBINER_TYPES
#include "PenumbraGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_TYPES

class PenumbraO0PreLegalizerCombinerImpl : public Combiner {
protected:
  const CombinerHelper Helper;
  const PenumbraO0PreLegalizerCombinerImplRuleConfig &RuleConfig;
  const PenumbraSubtarget &STI;

public:
  PenumbraO0PreLegalizerCombinerImpl(
      MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
      GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
      const PenumbraO0PreLegalizerCombinerImplRuleConfig &RuleConfig,
      const PenumbraSubtarget &STI);

  static const char *getName() { return "PenumbraO0PreLegalizerCombiner"; }

  bool tryCombineAll(MachineInstr &I) const override;

private:
#define GET_GICOMBINER_CLASS_MEMBERS
#include "PenumbraGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CLASS_MEMBERS
};

#define GET_GICOMBINER_IMPL
#include "PenumbraGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_IMPL

PenumbraO0PreLegalizerCombinerImpl::PenumbraO0PreLegalizerCombinerImpl(
    MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
    GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
    const PenumbraO0PreLegalizerCombinerImplRuleConfig &RuleConfig,
    const PenumbraSubtarget &STI)
    : Combiner(MF, CInfo, TPC, &VT, CSEInfo),
      Helper(Observer, B, /*IsPreLegalize*/ true, &VT), RuleConfig(RuleConfig),
      STI(STI),
#define GET_GICOMBINER_CONSTRUCTOR_INITS
#include "PenumbraGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CONSTRUCTOR_INITS
{
}

class PenumbraO0PreLegalizerCombiner : public MachineFunctionPass {
public:
  static char ID;

  PenumbraO0PreLegalizerCombiner();

  StringRef getPassName() const override {
    return "PenumbraO0PreLegalizerCombiner";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  PenumbraO0PreLegalizerCombinerImplRuleConfig RuleConfig;
};
} // end anonymous namespace

void PenumbraO0PreLegalizerCombiner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
  AU.setPreservesCFG();
  getSelectionDAGFallbackAnalysisUsage(AU);
  AU.addRequired<GISelValueTrackingAnalysisLegacy>();
  AU.addPreserved<GISelValueTrackingAnalysisLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

PenumbraO0PreLegalizerCombiner::PenumbraO0PreLegalizerCombiner()
    : MachineFunctionPass(ID) {
  if (!RuleConfig.parseCommandLineOption())
    report_fatal_error("Invalid rule identifier");
}

bool PenumbraO0PreLegalizerCombiner::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getProperties().hasFailedISel())
    return false;
  auto &TPC = getAnalysis<TargetPassConfig>();

  const Function &F = MF.getFunction();
  GISelValueTracking *VT =
      &getAnalysis<GISelValueTrackingAnalysisLegacy>().get(MF);

  const PenumbraSubtarget &ST = MF.getSubtarget<PenumbraSubtarget>();

  CombinerInfo CInfo(/*AllowIllegalOps*/ true, /*ShouldLegalizeIllegal*/ false,
                     /*LegalizerInfo*/ nullptr, /*EnableOpt*/ false,
                     F.hasOptSize(), F.hasMinSize());
  // Single-pass, no fixed-point iteration — at -O0 we trade missed combines
  // for compile-time predictability.
  CInfo.MaxIterations = 1;

  PenumbraO0PreLegalizerCombinerImpl Impl(MF, CInfo, &TPC, *VT,
                                          /*CSEInfo*/ nullptr, RuleConfig, ST);
  return Impl.combineMachineInstrs();
}

char PenumbraO0PreLegalizerCombiner::ID = 0;
INITIALIZE_PASS_BEGIN(PenumbraO0PreLegalizerCombiner, DEBUG_TYPE,
                      "Combine Penumbra machine instrs before legalization "
                      "(-O0)",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(GISelValueTrackingAnalysisLegacy)
INITIALIZE_PASS_END(PenumbraO0PreLegalizerCombiner, DEBUG_TYPE,
                    "Combine Penumbra machine instrs before legalization "
                    "(-O0)",
                    false, false)

namespace llvm {
FunctionPass *createPenumbraO0PreLegalizerCombiner() {
  return new PenumbraO0PreLegalizerCombiner();
}
} // namespace llvm

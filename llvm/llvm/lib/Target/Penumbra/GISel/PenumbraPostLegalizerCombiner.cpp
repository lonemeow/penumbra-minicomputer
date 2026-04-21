//=== PenumbraPostLegalizerCombiner.cpp -----------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Generic MachineIR combines run after the legalizer at -O1 and above.
// Combines here must preserve instruction legality for the Penumbra legalizer.
// Rule set is defined in PenumbraCombine.td.
//
//===----------------------------------------------------------------------===//

#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/CombinerInfo.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/TargetPassConfig.h"

#define GET_GICOMBINER_DEPS
#include "PenumbraGenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_DEPS

#define DEBUG_TYPE "penumbra-postlegalizer-combiner"

namespace llvm {
void initializePenumbraPostLegalizerCombinerPass(PassRegistry &);
} // namespace llvm

using namespace llvm;

namespace {

#define GET_GICOMBINER_TYPES
#include "PenumbraGenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_TYPES

// Match G_ADD / G_SUB whose RHS is a negative G_CONSTANT fitting
// [-65535, -1].  On success, MatchInfo holds the positive magnitude that
// the opposite opcode should use.
bool matchNegImmToOpposite(MachineInstr &MI, MachineRegisterInfo &MRI,
                           int64_t &MatchInfo) {
  unsigned Opc = MI.getOpcode();
  if (Opc != TargetOpcode::G_ADD && Opc != TargetOpcode::G_SUB)
    return false;

  auto Cst = getIConstantVRegSExtVal(MI.getOperand(2).getReg(), MRI);
  if (!Cst || *Cst >= 0)
    return false;
  int64_t Neg = -*Cst;
  if (!isUInt<16>(Neg))
    return false;

  MatchInfo = Neg;
  return true;
}

// Rewrite G_ADD→G_SUB / G_SUB→G_ADD with the negated constant.
void applyNegImmToOpposite(MachineInstr &MI, MachineRegisterInfo &MRI,
                           MachineIRBuilder &B, int64_t &MatchInfo) {
  B.setInstrAndDebugLoc(MI);
  unsigned NewOpc = MI.getOpcode() == TargetOpcode::G_ADD
                        ? TargetOpcode::G_SUB
                        : TargetOpcode::G_ADD;
  Register Dst = MI.getOperand(0).getReg();
  Register Src = MI.getOperand(1).getReg();
  LLT Ty = MRI.getType(Dst);
  auto NewCst = B.buildConstant(Ty, MatchInfo);
  B.buildInstr(NewOpc, {Dst}, {Src, NewCst.getReg(0)});
  MI.eraseFromParent();
}

class PenumbraPostLegalizerCombinerImpl : public Combiner {
protected:
  const CombinerHelper Helper;
  const PenumbraPostLegalizerCombinerImplRuleConfig &RuleConfig;
  const PenumbraSubtarget &STI;

public:
  PenumbraPostLegalizerCombinerImpl(
      MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
      GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
      const PenumbraPostLegalizerCombinerImplRuleConfig &RuleConfig,
      const PenumbraSubtarget &STI, MachineDominatorTree *MDT,
      const LegalizerInfo *LI);

  static const char *getName() { return "PenumbraPostLegalizerCombiner"; }

  bool tryCombineAll(MachineInstr &I) const override;

private:
#define GET_GICOMBINER_CLASS_MEMBERS
#include "PenumbraGenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CLASS_MEMBERS
};

#define GET_GICOMBINER_IMPL
#include "PenumbraGenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_IMPL

PenumbraPostLegalizerCombinerImpl::PenumbraPostLegalizerCombinerImpl(
    MachineFunction &MF, CombinerInfo &CInfo, const TargetPassConfig *TPC,
    GISelValueTracking &VT, GISelCSEInfo *CSEInfo,
    const PenumbraPostLegalizerCombinerImplRuleConfig &RuleConfig,
    const PenumbraSubtarget &STI, MachineDominatorTree *MDT,
    const LegalizerInfo *LI)
    : Combiner(MF, CInfo, TPC, &VT, CSEInfo),
      Helper(Observer, B, /*IsPreLegalize*/ false, &VT, MDT, LI),
      RuleConfig(RuleConfig), STI(STI),
#define GET_GICOMBINER_CONSTRUCTOR_INITS
#include "PenumbraGenPostLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CONSTRUCTOR_INITS
{
}

class PenumbraPostLegalizerCombiner : public MachineFunctionPass {
public:
  static char ID;

  PenumbraPostLegalizerCombiner();

  StringRef getPassName() const override {
    return "PenumbraPostLegalizerCombiner";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  PenumbraPostLegalizerCombinerImplRuleConfig RuleConfig;
};
} // end anonymous namespace

void PenumbraPostLegalizerCombiner::getAnalysisUsage(AnalysisUsage &AU) const {
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

PenumbraPostLegalizerCombiner::PenumbraPostLegalizerCombiner()
    : MachineFunctionPass(ID) {
  if (!RuleConfig.parseCommandLineOption())
    report_fatal_error("Invalid rule identifier");
}

bool PenumbraPostLegalizerCombiner::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getProperties().hasFailedISel())
    return false;
  assert(MF.getProperties().hasLegalized() &&
         "Expected a legalized function?");
  auto *TPC = &getAnalysis<TargetPassConfig>();
  const Function &F = MF.getFunction();
  bool EnableOpt =
      MF.getTarget().getOptLevel() != CodeGenOptLevel::None && !skipFunction(F);

  const PenumbraSubtarget &ST = MF.getSubtarget<PenumbraSubtarget>();
  const auto *LI = ST.getLegalizerInfo();

  GISelValueTracking *VT =
      &getAnalysis<GISelValueTrackingAnalysisLegacy>().get(MF);
  MachineDominatorTree *MDT =
      &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  GISelCSEAnalysisWrapper &Wrapper =
      getAnalysis<GISelCSEAnalysisWrapperPass>().getCSEWrapper();
  auto *CSEInfo = &Wrapper.get(TPC->getCSEConfig());

  CombinerInfo CInfo(/*AllowIllegalOps*/ true, /*ShouldLegalizeIllegal*/ false,
                     /*LegalizerInfo*/ nullptr, EnableOpt, F.hasOptSize(),
                     F.hasMinSize());
  PenumbraPostLegalizerCombinerImpl Impl(MF, CInfo, TPC, *VT, CSEInfo,
                                         RuleConfig, ST, MDT, LI);
  return Impl.combineMachineInstrs();
}

char PenumbraPostLegalizerCombiner::ID = 0;
INITIALIZE_PASS_BEGIN(PenumbraPostLegalizerCombiner, DEBUG_TYPE,
                      "Combine Penumbra MachineInstrs after legalization",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(GISelValueTrackingAnalysisLegacy)
INITIALIZE_PASS_END(PenumbraPostLegalizerCombiner, DEBUG_TYPE,
                    "Combine Penumbra MachineInstrs after legalization", false,
                    false)

namespace llvm {
FunctionPass *createPenumbraPostLegalizerCombiner() {
  return new PenumbraPostLegalizerCombiner();
}
} // namespace llvm

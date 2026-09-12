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
#include "llvm/CodeGen/GlobalISel/Utils.h"
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

// Match a G_SREM or G_SDIVREM whose divisor is a constant power of two that
// applySRemByPow2's rewrite computes correctly; Log2Divisor receives log2 of
// the divisor.
bool matchSRemByPow2(MachineInstr &MI, MachineRegisterInfo &MRI,
                     unsigned &Log2Divisor) {
  // Don't rewrite to a longer sequence when optimizing for size.
  if (MI.getMF()->getFunction().hasMinSize())
    return false;

  Register DivisorReg;
  switch (MI.getOpcode()) {
  case TargetOpcode::G_SREM:
    DivisorReg = MI.getOperand(2).getReg();
    break;
  case TargetOpcode::G_SDIVREM:
    DivisorReg = MI.getOperand(3).getReg();
    break;

  default:
    return false;
  }

  auto DivisorConst = getIConstantVRegValWithLookThrough(DivisorReg, MRI);
  if (!DivisorConst)
    return false;

  // The rewrite multiplies the quotient back by shifting, which equals the
  // true product exactly when the divisor's bit pattern is 1 << k.  That is
  // the unsigned power-of-two test: it accepts INT_MIN, where the shift wraps
  // to the divisor itself, and rejects a negated power of two, whose product
  // a left shift cannot form.
  APInt DivisorConstVal = DivisorConst.value().Value;
  if (!DivisorConstVal.isPowerOf2())
    return false;

  Log2Divisor = DivisorConstVal.logBase2();
  return true;
}

// Rewrite the remainder as X - ((X sdiv 2^k) << k).  The quotient stays a
// G_SDIV for sdiv_by_pow2 to expand, and a G_SDIVREM's quotient output is
// that same G_SDIV, so a divide/remainder pair computes one quotient.  No
// G_SREM survives the rewrite, so div_rem_to_divrem has nothing to re-fuse.
void applySRemByPow2(MachineInstr &MI, MachineRegisterInfo &MRI,
                     MachineIRBuilder &B, unsigned &Log2Divisor) {
  bool IsDivRem = MI.getOpcode() == TargetOpcode::G_SDIVREM;
  Register Rem = MI.getOperand(IsDivRem ? 1 : 0).getReg();
  Register X = MI.getOperand(IsDivRem ? 2 : 1).getReg();
  Register Divisor = MI.getOperand(IsDivRem ? 3 : 2).getReg();
  LLT Ty = MRI.getType(X);

  B.setInstrAndDebugLoc(MI);
  Register Quot = IsDivRem ? MI.getOperand(0).getReg()
                           : MRI.createGenericVirtualRegister(Ty);
  B.buildInstr(TargetOpcode::G_SDIV, {Quot}, {X, Divisor});
  const TargetLowering &TLI = *B.getMF().getSubtarget().getTargetLowering();
  auto ShiftAmt =
      B.buildConstant(TLI.getPreferredShiftAmountTy(Ty), Log2Divisor);
  auto Scaled = B.buildShl(Ty, Quot, ShiftAmt);
  B.buildSub(Rem, X, Scaled);

  MI.eraseFromParent();
}

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

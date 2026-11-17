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
#include "llvm/CodeGen/GlobalISel/MIPatternMatch.h"
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

// An operation one of whose operands is a select with the operation's
// identity in one arm.
struct SelectOfIdentity {
  unsigned SelectIdx;  // the operation's operand that holds the select
  bool IdentityIsTrue; // the identity is the select's true value
};

// Whether Reg holds a right identity of Opc: a value id with `x op id == x`
// for every x.  A divide has one too, but it traps on a zero divisor and so
// cannot sink into its arm: rewritten, it would run on the identity's path.
static bool isRightIdentity(unsigned Opc, Register Reg,
                            const MachineRegisterInfo &MRI) {
  using namespace MIPatternMatch;
  switch (Opc) {
  case TargetOpcode::G_ADD:
  case TargetOpcode::G_SUB:
  case TargetOpcode::G_OR:
  case TargetOpcode::G_XOR:
  case TargetOpcode::G_SHL:
  case TargetOpcode::G_LSHR:
  case TargetOpcode::G_ASHR:
  case TargetOpcode::G_PTR_ADD:
    return mi_match(Reg, MRI, m_ZeroInt());
  case TargetOpcode::G_AND:
    return mi_match(Reg, MRI, m_AllOnesInt());
  case TargetOpcode::G_MUL:
    return mi_match(Reg, MRI, m_SpecificICst(1));
  default:
    return false;
  }
}

// Match `x op select(c, id, k)`, or `select(c, id, k) op x` for a
// commutative operation.
bool matchSelectOfIdentity(MachineInstr &MI, MachineRegisterInfo &MRI,
                           SelectOfIdentity &MatchInfo) {
  // Register-width values only: the legalizer splits a wider operation into
  // selects of its own, which this must not reshape.
  if (MRI.getType(MI.getOperand(0).getReg()).getSizeInBits() != 32)
    return false;
  unsigned Opc = MI.getOpcode();
  for (unsigned Idx : {2u, 1u}) {
    if (Idx == 1 && !MI.isCommutable())
      break;
    Register Sel = MI.getOperand(Idx).getReg();
    MachineInstr *SelMI = MRI.getVRegDef(Sel);
    // The select must die here and be computed here: one from another
    // block, hoisted out of a loop say, would come back as a branch inside
    // it.
    if (!SelMI || SelMI->getOpcode() != TargetOpcode::G_SELECT ||
        !MRI.hasOneNonDBGUse(Sel) || SelMI->getParent() != MI.getParent())
      continue;
    for (bool IdentityIsTrue : {true, false}) {
      if (isRightIdentity(Opc, SelMI->getOperand(IdentityIsTrue ? 2 : 3).getReg(),
                          MRI)) {
        MatchInfo = {Idx, IdentityIsTrue};
        return true;
      }
    }
  }
  return false;
}

// Rewrite as `select(c, x, x op k)`, or with the arms swapped when the
// identity is the false value.  x goes first even when the select stood
// first, which only a commutative operation allows: the first operand is
// the one a two-address instruction overwrites, so x's register ends up
// holding the result on both paths.  The wrap flags are dropped: they held
// for the selected value, not for k.
void applySelectOfIdentity(MachineInstr &MI, MachineRegisterInfo &MRI,
                           MachineIRBuilder &B,
                           const SelectOfIdentity &MatchInfo) {
  B.setInstrAndDebugLoc(MI);
  MachineInstr &Sel =
      *MRI.getVRegDef(MI.getOperand(MatchInfo.SelectIdx).getReg());
  Register Dst = MI.getOperand(0).getReg();
  Register X = MI.getOperand(3 - MatchInfo.SelectIdx).getReg();
  Register K = Sel.getOperand(MatchInfo.IdentityIsTrue ? 3 : 2).getReg();
  Register Op =
      B.buildInstr(MI.getOpcode(), {MRI.getType(Dst)}, {X, K}).getReg(0);
  Register Cond = Sel.getOperand(1).getReg();
  if (MatchInfo.IdentityIsTrue)
    B.buildSelect(Dst, Cond, X, Op);
  else
    B.buildSelect(Dst, Cond, Op, X);
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

  bool tryCombineAllImpl(MachineInstr &I) const;

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

bool PenumbraPreLegalizerCombinerImpl::tryCombineAll(MachineInstr &MI) const {
  if (tryCombineAllImpl(MI))
    return true;

  switch (MI.getOpcode()) {
  case TargetOpcode::G_MEMCPY_INLINE:
    return Helper.tryEmitMemcpyInline(MI);
  case TargetOpcode::G_MEMCPY:
  case TargetOpcode::G_MEMMOVE:
  case TargetOpcode::G_MEMSET:
    // A constant length that fits the store limit in the access types
    // getOptimalMemOpLLT picks becomes loads and stores; the legalizer makes
    // anything else a library call, whose return gen2.5 never predicts and
    // across which only the callee-saved registers survive.
    return Helper.tryCombineMemCpyFamily(MI);
  default:
    return false;
  }
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

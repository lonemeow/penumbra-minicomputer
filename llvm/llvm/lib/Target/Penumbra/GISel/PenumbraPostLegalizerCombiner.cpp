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
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
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

// One term of a multiply by a constant rewritten as a sum of shifted copies of
// the multiplicand: +(x << Shift) or -(x << Shift).
struct MulTerm {
  unsigned Shift;
  bool Negative;
};

// C as a sum of +/-2^Shift terms, taken modulo 2^32 — the arithmetic the
// multiply computes — so a negative constant needs no special case and a term
// that would land at 2^32 vanishes.  The non-adjacent form (no two terms at
// adjacent shifts) has the fewest terms of any signed-digit representation,
// but its subtracts cost more than adds on a two-address machine: an add
// commutes into whichever operand is free, a subtract does not.  So the plain
// binary expansion wins unless the non-adjacent form is strictly shorter —
// 7 is 8 - 1, but 6 stays 4 + 2.
SmallVector<MulTerm, 16> getMulTerms(uint32_t C) {
  SmallVector<MulTerm, 16> NonAdjacent;
  // One bit wider than the constant, so rounding the top digit up can carry.
  uint64_t Rest = C;
  for (unsigned Shift = 0; Shift < 32 && Rest != 0; ++Shift, Rest >>= 1) {
    if (!(Rest & 1))
      continue;
    // Choose the digit that leaves the next bit clear.
    bool Negative = (Rest & 3) == 3;
    NonAdjacent.push_back({Shift, Negative});
    Rest = Negative ? Rest + 1 : Rest - 1;
  }
  if (NonAdjacent.size() < unsigned(llvm::popcount(C)))
    return NonAdjacent;

  SmallVector<MulTerm, 16> Binary;
  for (unsigned Shift = 0; Shift < 32; ++Shift)
    if (C >> Shift & 1)
      Binary.push_back({Shift, false});
  return Binary;
}

bool isMulDecompositionCheaper(unsigned Shifts, unsigned AddSubs,
                               const PenumbraSubtarget &STI, bool OptForSize) {
  // A single shift or negate is never larger than loading the constant and
  // multiplying; anything longer is.
  if (OptForSize)
    return Shifts + AddSubs <= 1;

  // LLI+LUI+MUL
  int MulCycles = STI.getMulCycles() + STI.getALUCycles() * 2;
  // Each shift also requires a MOV
  int DecompCycles = (Shifts * 2 + AddSubs) * STI.getALUCycles();

  double DecompCostRatio = (double)DecompCycles / MulCycles;

  // The decomposition adds I-cache pressure, so it can't be just marginally
  // cheaper to be a win
  return DecompCostRatio < 0.8;
}

// Match an s32 G_MUL by a constant whose shift/add decomposition is cheaper
// than the hardware multiply; MatchInfo receives the constant.
bool matchMulByConst(MachineInstr &MI, MachineRegisterInfo &MRI,
                     const PenumbraSubtarget &STI, uint32_t &MatchInfo) {
  Register X = MI.getOperand(1).getReg();
  Register Rhs = MI.getOperand(2).getReg();
  if (MRI.getType(MI.getOperand(0).getReg()) != LLT::scalar(32))
    return false;
  auto C = getIConstantVRegValWithLookThrough(Rhs, MRI);
  if (!C)
    return false;

  // A G_[SU]MULH on the same operands fuses with this multiply into one
  // MUL_P / MULU_P, whose low half comes for free.
  for (MachineInstr &Use : MRI.use_nodbg_instructions(X)) {
    unsigned Opc = Use.getOpcode();
    if ((Opc != TargetOpcode::G_UMULH && Opc != TargetOpcode::G_SMULH) ||
        Use.getParent() != MI.getParent())
      continue;
    Register UseLhs = Use.getOperand(1).getReg();
    Register UseRhs = Use.getOperand(2).getReg();
    if ((UseLhs == X && UseRhs == Rhs) || (UseLhs == Rhs && UseRhs == X))
      return false;
  }

  uint32_t Value = C->Value.getZExtValue();
  SmallVector<MulTerm, 16> Terms = getMulTerms(Value);
  unsigned Shifts = count_if(Terms, [](const MulTerm &T) { return T.Shift; });
  bool AnyPositive =
      any_of(Terms, [](const MulTerm &T) { return !T.Negative; });
  // One fewer add or subtract than terms, plus a negation when every term is
  // subtracted.
  unsigned AddSubs = Terms.empty() ? 0 : Terms.size() - 1 + !AnyPositive;
  if (!isMulDecompositionCheaper(Shifts, AddSubs, STI,
                                 MI.getMF()->getFunction().hasMinSize()))
    return false;

  MatchInfo = Value;
  return true;
}

// Emit the constant's terms as shifts of the multiplicand combined by adds and
// subtracts, starting from a positive term where there is one so the sum needs
// no negation.
void applyMulByConst(MachineInstr &MI, MachineRegisterInfo &MRI,
                     MachineIRBuilder &B, uint32_t &Value) {
  Register Dst = MI.getOperand(0).getReg();
  Register X = MI.getOperand(1).getReg();
  LLT Ty = MRI.getType(Dst);
  B.setInstrAndDebugLoc(MI);

  SmallVector<MulTerm, 16> Terms = getMulTerms(Value);
  if (Terms.empty()) {
    B.buildConstant(Dst, 0);
    MI.eraseFromParent();
    return;
  }

  auto Shifted = [&](const MulTerm &T) -> Register {
    if (T.Shift == 0)
      return X;
    return B.buildShl(Ty, X, B.buildConstant(Ty, T.Shift)).getReg(0);
  };

  auto *First = find_if(Terms, [](const MulTerm &T) { return !T.Negative; });
  Register Sum = First != Terms.end() ? Shifted(*First)
                                      : B.buildConstant(Ty, 0).getReg(0);
  for (const MulTerm &T : Terms) {
    if (&T == First)
      continue;
    unsigned Opc = T.Negative ? TargetOpcode::G_SUB : TargetOpcode::G_ADD;
    Sum = B.buildInstr(Opc, {Ty}, {Sum, Shifted(T)}).getReg(0);
  }
  B.buildCopy(Dst, Sum);

  MI.eraseFromParent();
}

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

// Match a plain `G_LOAD` whose memory size is smaller than its destination
// size — i.e. an extload — and promote it to `G_ZEXTLOAD`.  Penumbra's LDB
// and LDH always zero-extend in hardware, so this is a faithful description
// of the load's actual semantics.  After the promotion, GISelValueTracking
// reports the high bits as zero, which `redundant_and` then uses to drop
// the `G_AND %, 0xFF` masks the legalizer emits when widening unsigned /
// eq / ne `G_ICMP` operands.
bool matchZextloadPromote(MachineInstr &MI, MachineRegisterInfo &MRI) {
  // The combine root is matched on opcode by the rule (`G_LOAD`); other
  // load variants (G_ZEXTLOAD, G_SEXTLOAD) already carry their semantics.
  assert(MI.getOpcode() == TargetOpcode::G_LOAD);

  MachineMemOperand &MMO = **MI.memoperands_begin();
  Register DstReg = MI.getOperand(0).getReg();
  LLT Ty = MRI.getType(DstReg);
  
  if (!MMO.isUnordered())
    return false;

  if (!Ty.isScalar())
    return false;

  if (Ty.getSizeInBits() <= MMO.getMemoryType().getSizeInBits())
    return false;

  for (const MachineInstr &Use : MRI.use_nodbg_instructions(DstReg)) {
    if (Use.getOpcode() == TargetOpcode::G_SEXT ||
        Use.getOpcode() == TargetOpcode::G_SEXT_INREG)
      return false;
  }

  return true;
}

// Rewrite the matched load with G_ZEXTLOAD, reusing the same destination
// register, pointer, and memory operand.  The MMO is unchanged because the
// memory access size and alignment are identical — only the high-bit
// semantics differ between G_LOAD (anyext) and G_ZEXTLOAD (zero-extend).
void applyZextloadPromote(MachineInstr &MI, MachineRegisterInfo &MRI,
                          MachineIRBuilder &B) {
  B.setInstrAndDebugLoc(MI);
  Register Dst = MI.getOperand(0).getReg();
  Register Ptr = MI.getOperand(1).getReg();
  MachineMemOperand &MMO = **MI.memoperands_begin();
  B.buildLoadInstr(TargetOpcode::G_ZEXTLOAD, Dst, Ptr, MMO);
  MI.eraseFromParent();
}

// Skip past debug pseudo-instructions, which have no codegen effect.
static MachineInstr *skipDebugInstrs(MachineInstr *MI) {
  while (MI && MI->isDebugInstr())
    MI = MI->getNextNode();
  return MI;
}

// Match `G_PTR_ADD %new, %old, %k` whose immediately-following non-debug
// instruction is a memory op that reads `%old` (so sinking saves the
// trailing back-edge MOV) and does *not* read `%new` (otherwise we'd
// invert def-and-use).  Restricting to G_LOAD/G_STORE/G_{Z,S}EXTLOAD
// keeps the rule from running on neighboring G_PTR_ADD chains —
// `ptr_add_immed_chain` is what canonicalizes those.
bool matchSinkPtrAddPastUse(MachineInstr &MI, MachineRegisterInfo &MRI) {
  assert(MI.getOpcode() == TargetOpcode::G_PTR_ADD);

  // Match only when the immediately-following non-debug instruction is a
  // load/store that uses Src (%old) and not Dst (%new); walk its uses once
  // to record both flags.
  MachineInstr *Next = skipDebugInstrs(MI.getNextNode());
  if (!Next)
    return false;

  switch (Next->getOpcode()) {
    case TargetOpcode::G_LOAD:
    case TargetOpcode::G_STORE:
    case TargetOpcode::G_ZEXTLOAD:
    case TargetOpcode::G_SEXTLOAD:
      break;

    default:
      return false;
  }

  Register DstReg = MI.getOperand(0).getReg();
  Register SrcReg = MI.getOperand(1).getReg();

  bool usesSrc = false;
  for (auto &Use : Next->uses()) {
    if (!Use.isReg())
      continue;
    if (Use.getReg() == DstReg)
      return false;
    if (Use.getReg() == SrcReg) {
      usesSrc = true;
    }
  }

  return usesSrc;
}

// Rebuild the G_PTR_ADD at a position after the next instruction, reusing
// its dst register and operands.  Erasing the original definition fixes
// the brief two-defs-of-Dst window introduced by the build call — the
// pattern matches `applyZextloadPromote` / `applyNegImmToOpposite`.
void applySinkPtrAddPastUse(MachineInstr &MI, MachineRegisterInfo &MRI,
                            MachineIRBuilder &B) {
  Register Dst = MI.getOperand(0).getReg();
  Register Src = MI.getOperand(1).getReg();
  Register Off = MI.getOperand(2).getReg();

  MachineInstr *Next = skipDebugInstrs(MI.getNextNode());
  assert(Next && "matcher should have rejected when no next instruction");

  // Insert the rebuilt G_PTR_ADD right after Next.
  B.setInsertPt(*MI.getParent(), std::next(Next->getIterator()));
  B.setDebugLoc(MI.getDebugLoc());
  B.buildPtrAdd(Dst, Src, Off);

  MI.eraseFromParent();
}

// Match a G_[SU]MULH whose operand pair is also multiplied by a G_MUL in
// the same basic block; MatchInfo receives that sibling G_MUL.  The low
// half of a product is signedness-agnostic, so one G_MUL can pair with
// either MULH flavor.
bool matchMulToMulLoHi(MachineInstr &MI, MachineRegisterInfo &MRI,
                       MachineInstr *&MatchInfo) {
  Register Multiplicand = MI.getOperand(1).getReg();
  Register Multiplier = MI.getOperand(2).getReg();
  for (MachineInstr &Potential : MRI.use_nodbg_instructions(Multiplicand)) {
    if (Potential.getOpcode() != TargetOpcode::G_MUL ||
        Potential.getParent() != MI.getParent())
      continue;
    // Multiplication commutes, so either operand order computes the same
    // low half.
    Register Lhs = Potential.getOperand(1).getReg();
    Register Rhs = Potential.getOperand(2).getReg();
    if ((Lhs == Multiplicand && Rhs == Multiplier) ||
        (Lhs == Multiplier && Rhs == Multiplicand)) {
      MatchInfo = &Potential;
      return true;
    }
  }

  return false;
}

// Fuse the pair into one two-result G_[SU]MUL_LOHI (selects to MUL_P /
// MULU_P), inserted at whichever of the two comes first in the block so
// both results dominate all existing uses.
void applyMulToMulLoHi(MachineInstr &MI, MachineRegisterInfo &MRI,
                       MachineIRBuilder &B, MachineInstr *&MulMI) {
  unsigned Opc = MI.getOpcode() == TargetOpcode::G_SMULH
                     ? Penumbra::G_SMUL_LOHI
                     : Penumbra::G_UMUL_LOHI;

  MachineInstr *First = MulMI;
  for (MachineInstr &Cur : *MI.getParent()) {
    if (&Cur == &MI) {
      First = &MI;
      break;
    }
    if (&Cur == MulMI)
      break;
  }

  B.setInsertPt(*First->getParent(), First->getIterator());
  B.setDebugLoc(First->getDebugLoc());
  B.buildInstr(Opc,
               {MulMI->getOperand(0).getReg(), MI.getOperand(0).getReg()},
               {MI.getOperand(1).getReg(), MI.getOperand(2).getReg()});

  MulMI->eraseFromParent();
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

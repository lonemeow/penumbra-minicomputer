//===-- PenumbraInstructionSelector.cpp - Penumbra Instruction Selector ---===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PenumbraFixupKinds.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraMachineFunctionInfo.h"
#include "PenumbraRegisterBankInfo.h"
#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGenTypes/LowLevelType.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "penumbra-isel"

using namespace llvm;

#define GET_GLOBALISEL_PREDICATE_BITSET
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATE_BITSET

namespace {

class PenumbraInstructionSelector : public InstructionSelector {
public:
  PenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                               const PenumbraSubtarget &STI,
                               const PenumbraRegisterBankInfo &RBI);

  bool select(MachineInstr &I) override;
  static const char *getName() { return DEBUG_TYPE; }

  void setupMF(MachineFunction &MF, GISelValueTracking *VT,
               CodeGenCoverage *CoverageInfo, ProfileSummaryInfo *PSI,
               BlockFrequencyInfo *BFI) override {
    InstructionSelector::setupMF(MF, VT, CoverageInfo, PSI, BFI);
    MRI = &MF.getRegInfo();
  }

private:
  // TableGen-generated pattern matcher.
  bool selectImpl(MachineInstr &I, CodeGenCoverage &CoverageInfo) const;

  // ComplexPattern entry point for LDW/STW/LDH/STH/LDB/STB address operands.
  // Decomposes the address into (base, offset16) so TableGen load/store
  // patterns can fold a constant GEP offset or a frame index into the
  // instruction's Rb/offset slots.  Bound to the AddrRegImm ComplexPattern
  // via GIComplexOperandMatcher in PenumbraGISel.td.
  ComplexRendererFns selectAddrRegImm(MachineOperand &Root) const;

  bool selectConstant(MachineInstr &I, MachineBasicBlock &MBB,
                      MachineRegisterInfo &MRI) const;
  bool selectLoad(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectStore(MachineInstr &I, MachineBasicBlock &MBB,
                   MachineRegisterInfo &MRI) const;
  bool selectFrameIndex(MachineInstr &I, MachineBasicBlock &MBB,
                        MachineRegisterInfo &MRI) const;
  bool selectBranch(MachineInstr &I, MachineBasicBlock &MBB) const;
  bool selectBrCond(MachineInstr &I, MachineBasicBlock &MBB,
                    MachineRegisterInfo &MRI) const;
  bool selectSelect(MachineInstr &I, MachineBasicBlock &MBB,
                    MachineRegisterInfo &MRI) const;
  bool selectICmp(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectZExt(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectSExt(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectGlobalValue(MachineInstr &I, MachineBasicBlock &MBB,
                         MachineRegisterInfo &MRI) const;
  bool selectBlockAddress(MachineInstr &I, MachineBasicBlock &MBB,
                          MachineRegisterInfo &MRI) const;
  bool selectJumpTable(MachineInstr &I, MachineBasicBlock &MBB,
                       MachineRegisterInfo &MRI) const;
  bool selectBrJT(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectIntrinsic(MachineInstr &I, MachineBasicBlock &MBB,
                       MachineRegisterInfo &MRI) const;

  // Emit LLI+LUI pair to materialise a symbol address into DstReg.
  // LoOp/HiOp are the lo16/hi16 operands (GlobalAddress, JumpTableIndex, etc.)
  void emitLoadSymbolAddr(Register DstReg, const DebugLoc &DL,
                          MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator InsertPt,
                          const MachineOperand &LoOp,
                          const MachineOperand &HiOp) const;

  const PenumbraTargetMachine &TM;
  const PenumbraInstrInfo &TII;
  const PenumbraRegisterInfo &TRI;
  const PenumbraRegisterBankInfo &RBI;

  // Required by generated selector code (references Subtarget->).
  const PenumbraSubtarget *Subtarget;

  // Populated by setupMF() — needed by ComplexPattern helpers that get only
  // a MachineOperand and must walk the def graph.
  MachineRegisterInfo *MRI = nullptr;

#define GET_GLOBALISEL_PREDICATES_DECL
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATES_DECL

#define GET_GLOBALISEL_TEMPORARIES_DECL
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_TEMPORARIES_DECL
};

} // end anonymous namespace

#define GET_GLOBALISEL_IMPL
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_IMPL

PenumbraInstructionSelector::PenumbraInstructionSelector(
    const PenumbraTargetMachine &TM, const PenumbraSubtarget &STI,
    const PenumbraRegisterBankInfo &RBI)
    : InstructionSelector(), TM(TM), TII(*STI.getInstrInfo()),
      TRI(*STI.getRegisterInfo()), RBI(RBI), Subtarget(&STI),
#define GET_GLOBALISEL_PREDICATES_INIT
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATES_INIT
#define GET_GLOBALISEL_TEMPORARIES_INIT
#include "PenumbraGenGlobalISel.inc"
#undef GET_GLOBALISEL_TEMPORARIES_INIT
{
}

bool PenumbraInstructionSelector::select(MachineInstr &I) {
  // Handle non-generic (already selected) instructions, plus G_PHI which is
  // "copy-like": despite being a generic opcode, G_PHI doesn't need full
  // instruction selection — it just gets its opcode changed to the target PHI
  // and its def constrained to a register class.  COPY similarly needs its
  // dest constrained from a register bank to a concrete register class.
  if (!isPreISelGenericOpcode(I.getOpcode()) ||
      I.getOpcode() == TargetOpcode::G_PHI) {
    if (I.getOpcode() == TargetOpcode::G_PHI) {
      I.setDesc(TII.get(TargetOpcode::PHI));
      return RBI.constrainGenericRegister(
          I.getOperand(0).getReg(), Penumbra::GPR_AllocatableRegClass,
          I.getParent()->getParent()->getRegInfo());
    }

    // COPY: constrain the dest register to a proper register class.
    if (I.isCopy()) {
      Register DstReg = I.getOperand(0).getReg();
      if (DstReg.isVirtual())
        return RBI.constrainGenericRegister(
            DstReg, Penumbra::GPR_AllocatableRegClass,
            I.getParent()->getParent()->getRegInfo());
    }

    return true;
  }

  MachineBasicBlock &MBB = *I.getParent();
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();

  using namespace TargetOpcode;

  // Route loads/stores with frame-index base to manual selection BEFORE
  // selectImpl — the manual path folds the FI into the memory instruction's
  // base operand, avoiding a separate LEAfi materialization.
  // Store-zero (R0 substitution) is handled automatically by the generated
  // selector via GIZeroRegister on the GPRz store data operand.
  if (I.getOpcode() == G_LOAD || I.getOpcode() == G_STORE) {
    Register AddrReg = I.getOperand(1).getReg();
    MachineInstr *AddrDef = MRI.getVRegDef(AddrReg);
    if (AddrDef && AddrDef->getOpcode() == G_FRAME_INDEX) {
      if (I.getOpcode() == G_LOAD)
        return selectLoad(I, MBB, MRI);
      else
        return selectStore(I, MBB, MRI);
    }
  }

  // Try TableGen-generated patterns (ALU, shifts, constants, loads/stores).
  if (selectImpl(I, *CoverageInfo))
    return true;

  switch (I.getOpcode()) {
  // ── Pointer arithmetic ────────────────────────────────────────────────────
  // G_PTR_ADD is handled by TableGen patterns in PenumbraGISel.td (both
  // reg-reg and reg-imm forms — see the `ptradd` patterns there).
  // G_PTRMASK stays here because we don't yet have a p0 TableGen equivalent
  // for SDAG-style (and p0, ...) matching.
  case G_PTRMASK: {
    MachineInstr *NewI =
        BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::AND))
            .addDef(I.getOperand(0).getReg())
            .addReg(I.getOperand(1).getReg())
            .addReg(I.getOperand(2).getReg());
    I.eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // ── Constants / Addresses ─────────────────────────────────────────────────
  case G_CONSTANT:     return selectConstant(I, MBB, MRI);
  case G_GLOBAL_VALUE: return selectGlobalValue(I, MBB, MRI);
  case G_BLOCK_ADDR:   return selectBlockAddress(I, MBB, MRI);

  // ── Memory ────────────────────────────────────────────────────────────────
  // G_LOAD/G_STORE: handled by pre-check (FI fold / store-zero) or selectImpl.
  // Only G_LOAD/G_STORE that fall through here are bugs (selectImpl should
  // have matched them).
  case G_FRAME_INDEX: return selectFrameIndex(I, MBB, MRI);
  case G_JUMP_TABLE:  return selectJumpTable(I, MBB, MRI);
  case G_BRJT:        return selectBrJT(I, MBB, MRI);

  // ── Branches ──────────────────────────────────────────────────────────────
  case G_BR:     return selectBranch(I, MBB);
  case G_BRCOND: return selectBrCond(I, MBB, MRI);
  case G_FENCE:
    // Uniprocessor: no hardware barrier needed, just a compiler barrier.
    BuildMI(MBB, I, I.getDebugLoc(), TII.get(TargetOpcode::MEMBARRIER));
    I.eraseFromParent();
    return true;
  case G_TRAP:
  case G_DEBUGTRAP:
    BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::BREAK));
    I.eraseFromParent();
    return true;
  case G_BRINDIRECT: {
    auto MI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::BRIND))
                  .addReg(I.getOperand(0).getReg());
    constrainSelectedInstRegOperands(*MI, TII, TRI, RBI);
    I.eraseFromParent();
    return true;
  }

  // ── Extensions / Truncation ─────────────────────────────────────────────────
  case G_ANYEXT:
  case G_TRUNC:
  case G_INTTOPTR:
  case G_PTRTOINT:
  case G_FREEZE:
    I.setDesc(TII.get(TargetOpcode::COPY));
    return RBI.constrainGenericRegister(
        I.getOperand(0).getReg(), Penumbra::GPR_AllocatableRegClass, MRI);

  case G_IMPLICIT_DEF:
    I.setDesc(TII.get(TargetOpcode::IMPLICIT_DEF));
    return RBI.constrainGenericRegister(
        I.getOperand(0).getReg(), Penumbra::GPR_AllocatableRegClass, MRI);

  case G_ZEXT: return selectZExt(I, MBB, MRI);
  case G_SEXT: return selectSExt(I, MBB, MRI);

  // ── Compare / Select ────────────────────────────────────────────────────────
  case G_ICMP:   return selectICmp(I, MBB, MRI);
  case G_SELECT: return selectSelect(I, MBB, MRI);

  // ── Stack save/restore (alloca) ─────────────────────────────────────────────
  case G_STACKSAVE: {
    auto NewI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::MOV))
        .addDef(I.getOperand(0).getReg())
        .addReg(Penumbra::R14);
    I.eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }
  case G_STACKRESTORE: {
    auto NewI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::MOV))
        .addDef(Penumbra::R14)
        .addReg(I.getOperand(0).getReg());
    I.eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // ── Intrinsics ──────────────────────────────────────────────────────────────
  case G_INTRINSIC: return selectIntrinsic(I, MBB, MRI);
  case G_INTRINSIC_W_SIDE_EFFECTS: return selectIntrinsic(I, MBB, MRI);

  default:
    return false;
  }
}

// ── AddrRegImm ComplexPattern ────────────────────────────────────────────────
// Decomposes a load/store address into (base, simm16_offset) for the Rb /
// offset slots of LDW/STW/LDH/STH/LDB/STB.  Bound to the AddrRegImm
// ComplexPattern via GIComplexOperandMatcher in PenumbraGISel.td.
InstructionSelector::ComplexRendererFns
PenumbraInstructionSelector::selectAddrRegImm(MachineOperand &Root) const {
  // Root must be a vreg — the call to getVRegDef below needs getReg() to be
  // valid.  A non-register operand here would be unusual (G_LOAD/G_STORE
  // address operands are always vregs in legalized MIR) but the guard lets
  // the matcher cleanly skip to the next pattern rather than asserting.
  if (!Root.isReg())
    return std::nullopt;

  MachineInstr *RootDef = MRI->getVRegDef(Root.getReg());

  if (RootDef->getOpcode() == TargetOpcode::G_FRAME_INDEX) {
    return {{
        [=](MachineInstrBuilder &MIB) { MIB.add(RootDef->getOperand(1)); },
        [=](MachineInstrBuilder &MIB) { MIB.addImm(0); },
    }};
  }

  if (isBaseWithConstantOffset(Root, *MRI)) {
    MachineOperand &LHS = RootDef->getOperand(1);
    MachineOperand &RHS = RootDef->getOperand(2);
    MachineInstr *LHSDef = MRI->getVRegDef(LHS.getReg());
    MachineInstr *RHSDef = MRI->getVRegDef(RHS.getReg());

    int64_t RHSC = RHSDef->getOperand(1).getCImm()->getSExtValue();
    if (isInt<16>(RHSC)) {
      if (LHSDef->getOpcode() == TargetOpcode::G_FRAME_INDEX)
        return {{
            [=](MachineInstrBuilder &MIB) { MIB.add(LHSDef->getOperand(1)); },
            [=](MachineInstrBuilder &MIB) { MIB.addImm(RHSC); },
        }};

      return {{[=](MachineInstrBuilder &MIB) { MIB.add(LHS); },
               [=](MachineInstrBuilder &MIB) { MIB.addImm(RHSC); }}};
    }
  }

  // Fall back to the fully materialized address with no offset
  return {{[=](MachineInstrBuilder &MIB) { MIB.addReg(Root.getReg()); },
           [=](MachineInstrBuilder &MIB) { MIB.addImm(0); }}};
}

// ── G_CONSTANT (wide only) ───────────────────────────────────────────────────
// selectImpl handles LLI (0–65535) and LLIS (-32768–-1) via TableGen patterns.
// This manual path only handles values that need two instructions: LLI + LUI.
bool PenumbraInstructionSelector::selectConstant(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  int64_t Val = I.getOperand(1).getCImm()->getSExtValue();
  const DebugLoc &DL = I.getDebugLoc();

  // Use a fresh vreg for LLI so each vreg has exactly one def (SSA).
  Register TmpReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  auto MI1 = BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
      .addDef(TmpReg)
      .addImm(Val & 0xFFFF);
  constrainSelectedInstRegOperands(*MI1, TII, TRI, RBI);
  auto MI2 = BuildMI(MBB, I, DL, TII.get(Penumbra::LUI))
      .addDef(DstReg)
      .addReg(TmpReg)
      .addImm((Val >> 16) & 0xFFFF);
  constrainSelectedInstRegOperands(*MI2, TII, TRI, RBI);

  I.eraseFromParent();
  return true;
}

// ── Memory helpers ────────────────────────────────────────────────────────────
//
// G_LOAD / G_STORE select into LDW / STW.
//
// Frame-index folding: when the address register is defined by G_FRAME_INDEX,
// we embed the FI directly into the LDW/STW Rb operand instead of materialising
// it as a separate register.  eliminateFrameIndex then patches the FI → R14 and
// fills in the real stack offset, matching the (GPR:$Rb, i32imm:$offset) layout
// of our memory instructions:
//
//   LDW:  (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset)
//           operand 0 = dst   operand 1 = Rb (FI here)  operand 2 = offset
//   STW:  (outs),           (ins GPR:$Rd, GPR:$Rb, i32imm:$offset)
//           operand 0 = val   operand 1 = Rb (FI here)  operand 2 = offset

bool PenumbraInstructionSelector::selectLoad(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register DstReg  = I.getOperand(0).getReg();
  Register AddrReg = I.getOperand(1).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  LLT DstTy = MRI.getType(DstReg);
  if (DstTy != LLT::scalar(32) && DstTy != LLT::pointer(0, 32))
    return false; // only s32/p0 for now

  // Select LDW/LDH/LDB based on memory operand size.
  unsigned MemSize = I.memoperands().front()->getSize().getValue();
  unsigned Opc;
  switch (MemSize) {
  case 4: Opc = Penumbra::LDW; break;
  case 2: Opc = Penumbra::LDH; break;
  case 1: Opc = Penumbra::LDB; break;
  default: return false;
  }

  auto MIB = BuildMI(MBB, I, DL, TII.get(Opc)).addDef(DstReg);

  MachineInstr *AddrDef = MRI.getVRegDef(AddrReg);
  if (AddrDef && AddrDef->getOpcode() == TargetOpcode::G_FRAME_INDEX)
    MIB.addFrameIndex(AddrDef->getOperand(1).getIndex());
  else
    MIB.addReg(AddrReg);
  MIB.addImm(0);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*MIB, TII, TRI, RBI);
}

bool PenumbraInstructionSelector::selectStore(MachineInstr &I,
                                               MachineBasicBlock &MBB,
                                               MachineRegisterInfo &MRI) const {
  Register ValReg  = I.getOperand(0).getReg();
  Register AddrReg = I.getOperand(1).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  LLT ValTy = MRI.getType(ValReg);
  if (ValTy != LLT::scalar(32) && ValTy != LLT::pointer(0, 32))
    return false;

  // Select STW/STH/STB based on memory operand size.
  unsigned MemSize = I.memoperands().front()->getSize().getValue();
  unsigned StOpc;
  switch (MemSize) {
  case 4: StOpc = Penumbra::STW; break;
  case 2: StOpc = Penumbra::STH; break;
  case 1: StOpc = Penumbra::STB; break;
  default: return false;
  }

  // This manual path is only reached for frame-index-folded stores.
  // Non-FI stores (including store-zero with R0 substitution via GIZeroRegister)
  // are handled by selectImpl's TableGen patterns.
  auto MIB = BuildMI(MBB, I, DL, TII.get(StOpc)).addReg(ValReg);

  MachineInstr *AddrDef = MRI.getVRegDef(AddrReg);
  if (AddrDef && AddrDef->getOpcode() == TargetOpcode::G_FRAME_INDEX)
    MIB.addFrameIndex(AddrDef->getOperand(1).getIndex());
  else
    MIB.addReg(AddrReg);
  MIB.addImm(0);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*MIB, TII, TRI, RBI);
}

bool PenumbraInstructionSelector::selectFrameIndex(MachineInstr &I,
                                                    MachineBasicBlock &MBB,
                                                    MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  int FI = I.getOperand(1).getIndex();

  if (MRI.use_nodbg_empty(DstReg)) {
    // All uses were folded into memory instructions — just erase.
    I.eraseFromParent();
    return true;
  }

  // Address escapes (e.g. passed to a function): materialise SP + offset
  // via LEAfi pseudo.  eliminateFrameIndex will resolve the FI → R14 and
  // fill in the real offset; expandPostRAPseudo expands to MOV + ADDi.
  MachineInstr *NewI =
      BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::LEAfi))
          .addDef(DstReg)
          .addFrameIndex(FI)
          .addImm(0);
  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── ICMP predicate → Penumbra branch opcode ──────────────────────────────────
// Maps LLVM ICMP predicates to the Penumbra conditional branch that tests the
// same condition after a CMP.  Returns 0 for unsupported predicates.
static unsigned icmpPredToBranchOpc(CmpInst::Predicate Pred) {
  switch (Pred) {
  case CmpInst::ICMP_EQ:  return Penumbra::BEQ;
  case CmpInst::ICMP_NE:  return Penumbra::BNE;
  case CmpInst::ICMP_SGT: return Penumbra::BGT;
  case CmpInst::ICMP_SGE: return Penumbra::BGE;
  case CmpInst::ICMP_SLT: return Penumbra::BLT;
  case CmpInst::ICMP_SLE: return Penumbra::BLE;
  case CmpInst::ICMP_UGT: return Penumbra::BHI;
  case CmpInst::ICMP_UGE: return Penumbra::BCS;
  case CmpInst::ICMP_ULT: return Penumbra::BCC;
  case CmpInst::ICMP_ULE: return Penumbra::BLS;
  default:                 return 0;
  }
}

// ── G_BR (unconditional branch) ──────────────────────────────────────────────
bool PenumbraInstructionSelector::selectBranch(MachineInstr &I,
                                                MachineBasicBlock &MBB) const {
  BuildMI(MBB, I, I.getDebugLoc(), TII.get(Penumbra::B))
      .addMBB(I.getOperand(0).getMBB());
  I.eraseFromParent();
  return true;
}

// ── G_BRCOND (conditional branch) ────────────────────────────────────────────
// Folds with the G_ICMP that defines the condition:
//   G_ICMP s1 %cond = pred, %lhs, %rhs   →   CMP %lhs, %rhs
//   G_BRCOND %cond, %target               →   Bcc %target
// If the condition is not from G_ICMP, falls back to TEST + BNE (nonzero).
bool PenumbraInstructionSelector::selectBrCond(MachineInstr &I,
                                                MachineBasicBlock &MBB,
                                                MachineRegisterInfo &MRI) const {
  Register CondReg = I.getOperand(0).getReg();
  MachineBasicBlock *TargetMBB = I.getOperand(1).getMBB();
  const DebugLoc &DL = I.getDebugLoc();

  MachineInstr *CondDef = MRI.getVRegDef(CondReg);

  if (CondDef && CondDef->getOpcode() == TargetOpcode::G_ICMP) {
    // Fold G_ICMP + G_BRCOND into CMP + Bcc.
    auto Pred = static_cast<CmpInst::Predicate>(
        CondDef->getOperand(1).getPredicate());
    Register LHS = CondDef->getOperand(2).getReg();
    Register RHS = CondDef->getOperand(3).getReg();

    // Canonicalize: if the LHS is constant and RHS is not, swap operands and
    // invert the predicate so the constant lands on the RHS where CMPi can
    // fold it.  Belt-and-braces with the upstream canonicalizer.
    MachineInstr *LhsDef = MRI.getVRegDef(LHS);
    MachineInstr *RhsDef = MRI.getVRegDef(RHS);
    if (LhsDef && LhsDef->getOpcode() == TargetOpcode::G_CONSTANT &&
        (!RhsDef || RhsDef->getOpcode() != TargetOpcode::G_CONSTANT)) {
      std::swap(LHS, RHS);
      std::swap(LhsDef, RhsDef);
      Pred = CmpInst::getSwappedPredicate(Pred);
    }

    unsigned BrOpc = icmpPredToBranchOpc(Pred);
    if (!BrOpc)
      return false;

    // If RHS is a constant that fits uimm16, emit CMPi — saves an LLI and a
    // live register.  Covers the common cases: `x == 0`, `n < 100`, loop
    // termination checks, etc.
    if (RhsDef && RhsDef->getOpcode() == TargetOpcode::G_CONSTANT &&
        isUInt<16>(RhsDef->getOperand(1).getCImm()->getZExtValue())) {
      int64_t Imm = RhsDef->getOperand(1).getCImm()->getZExtValue();
      auto CmpMI = BuildMI(MBB, I, DL, TII.get(Penumbra::CMPi))
                       .addReg(LHS)
                       .addImm(Imm);
      constrainSelectedInstRegOperands(*CmpMI, TII, TRI, RBI);
      if (MRI.use_nodbg_empty(RHS))
        RhsDef->eraseFromParent();
    } else {
      auto CmpMI = BuildMI(MBB, I, DL, TII.get(Penumbra::CMP))
                       .addReg(LHS)
                       .addReg(RHS);
      constrainSelectedInstRegOperands(*CmpMI, TII, TRI, RBI);
    }

    BuildMI(MBB, I, DL, TII.get(BrOpc)).addMBB(TargetMBB);

    I.eraseFromParent();
    // If G_ICMP has no remaining uses, erase it too.
    if (MRI.use_nodbg_empty(CondDef->getOperand(0).getReg()))
      CondDef->eraseFromParent();

    return true;
  }

  // Fallback: condition is a generic s1/s32 value — TEST reg, reg + BNE.
  auto TestMI = BuildMI(MBB, I, DL, TII.get(Penumbra::TEST))
                    .addReg(CondReg)
                    .addReg(CondReg);
  constrainSelectedInstRegOperands(*TestMI, TII, TRI, RBI);

  BuildMI(MBB, I, DL, TII.get(Penumbra::BNE)).addMBB(TargetMBB);
  I.eraseFromParent();
  return true;
}

// ── G_SELECT (conditional select) ────────────────────────────────────────────
// When the condition comes from G_ICMP, fold the comparison into SELECT_CC_GPR
// (CMP+Bcc in the diamond).  Otherwise fall back to SELECT_GPR (TEST+BNE).
bool PenumbraInstructionSelector::selectSelect(MachineInstr &I,
                                                MachineBasicBlock &MBB,
                                                MachineRegisterInfo &MRI) const {
  Register DstReg   = I.getOperand(0).getReg();
  Register CondReg  = I.getOperand(1).getReg();
  Register TrueReg  = I.getOperand(2).getReg();
  Register FalseReg = I.getOperand(3).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  MachineInstr *CondDef = MRI.getVRegDef(CondReg);

  // Fold G_ICMP condition into SELECT_CC_GPR: CMP+Bcc instead of TEST+BNE.
  if (CondDef && CondDef->getOpcode() == TargetOpcode::G_ICMP) {
    auto Pred = static_cast<CmpInst::Predicate>(
        CondDef->getOperand(1).getPredicate());
    unsigned BrOpc = icmpPredToBranchOpc(Pred);
    if (!BrOpc)
      return false;

    Register LHS = CondDef->getOperand(2).getReg();
    Register RHS = CondDef->getOperand(3).getReg();

    MachineInstr *NewI =
        BuildMI(MBB, I, DL, TII.get(Penumbra::SELECT_CC_GPR))
            .addDef(DstReg)
            .addReg(TrueReg)
            .addReg(FalseReg)
            .addReg(LHS)
            .addReg(RHS)
            .addImm(BrOpc);

    I.eraseFromParent();
    if (MRI.use_nodbg_empty(CondDef->getOperand(0).getReg()))
      CondDef->eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // Generic condition: SELECT_GPR with TEST+BNE.
  MachineInstr *NewI =
      BuildMI(MBB, I, DL, TII.get(Penumbra::SELECT_GPR))
          .addDef(DstReg)
          .addReg(TrueReg)
          .addReg(FalseReg)
          .addReg(CondReg);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── G_ICMP (standalone comparison → 0/1 value) ──────────────────────────────
// When G_ICMP is consumed by G_BRCOND or G_SELECT, those handlers fold it away.
// If it survives (e.g. `int x = a > b;`), materialize 0/1 via SELECT_CC_GPR.
bool PenumbraInstructionSelector::selectICmp(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  // Already consumed by a fold?
  if (MRI.use_nodbg_empty(I.getOperand(0).getReg())) {
    I.eraseFromParent();
    return true;
  }

  Register DstReg = I.getOperand(0).getReg();
  auto Pred = static_cast<CmpInst::Predicate>(
      I.getOperand(1).getPredicate());
  Register LHS = I.getOperand(2).getReg();
  Register RHS = I.getOperand(3).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  unsigned BrOpc = icmpPredToBranchOpc(Pred);
  if (!BrOpc)
    return false;

  // Materialize constants 1 (true) and 0 (false).
  Register OneReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  Register ZeroReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, I, DL, TII.get(Penumbra::LLI)).addDef(OneReg).addImm(1);
  BuildMI(MBB, I, DL, TII.get(Penumbra::LLI)).addDef(ZeroReg).addImm(0);

  MachineInstr *NewI =
      BuildMI(MBB, I, DL, TII.get(Penumbra::SELECT_CC_GPR))
          .addDef(DstReg)
          .addReg(OneReg)
          .addReg(ZeroReg)
          .addReg(LHS)
          .addReg(RHS)
          .addImm(BrOpc);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── G_ZEXT (zero-extend) ─────────────────────────────────────────────────────
// s1→s32: AND #1, s8→s32: AND #0xFF, s16→s32: AND #0xFFFF
bool PenumbraInstructionSelector::selectZExt(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  Register SrcReg = I.getOperand(1).getReg();
  unsigned SrcBits = MRI.getType(SrcReg).getSizeInBits();
  const DebugLoc &DL = I.getDebugLoc();

  uint64_t Mask = (1ULL << SrcBits) - 1; // 1, 0xFF, or 0xFFFF
  MachineInstr *NewI =
      BuildMI(MBB, I, DL, TII.get(Penumbra::ANDi))
          .addDef(DstReg)
          .addReg(SrcReg)
          .addImm(Mask);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── G_SEXT (sign-extend) ────────────────────────────────────────────────────
// Shift left to put the sign bit at bit 31, then arithmetic shift right.
// s1→s32: SHL 31 + SAR 31, s8→s32: SHL 24 + SAR 24, s16→s32: SHL 16 + SAR 16
bool PenumbraInstructionSelector::selectSExt(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  Register SrcReg = I.getOperand(1).getReg();
  unsigned SrcBits = MRI.getType(SrcReg).getSizeInBits();
  const DebugLoc &DL = I.getDebugLoc();

  unsigned ShAmt = 32 - SrcBits;
  Register TmpReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, I, DL, TII.get(Penumbra::SHLi))
      .addDef(TmpReg)
      .addReg(SrcReg)
      .addImm(ShAmt);
  MachineInstr *NewI =
      BuildMI(MBB, I, DL, TII.get(Penumbra::SARi))
          .addDef(DstReg)
          .addReg(TmpReg)
          .addImm(ShAmt);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── LLI+LUI symbol address helper ────────────────────────────────────────────
// Shared by G_GLOBAL_VALUE, G_JUMP_TABLE, and future symbol materialisations.
// Emits:  LLI Rd, :lo16:sym  /  LUI Rd, Rd, :hi16:sym
// Caller provides the lo16/hi16 MachineOperands (GlobalAddress, JTI, etc.).
void PenumbraInstructionSelector::emitLoadSymbolAddr(
    Register DstReg, const DebugLoc &DL, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator InsertPt,
    const MachineOperand &LoOp, const MachineOperand &HiOp) const {
  // Use a fresh vreg for LLI to maintain SSA (one def per vreg).
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  Register TmpReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);

  auto LLIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LLI))
      .addDef(TmpReg)
      .add(LoOp);
  constrainSelectedInstRegOperands(*LLIInst, TII, TRI, RBI);

  auto LUIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LUI))
      .addDef(DstReg)
      .addReg(TmpReg)
      .add(HiOp);
  constrainSelectedInstRegOperands(*LUIInst, TII, TRI, RBI);
}

// ── G_GLOBAL_VALUE ───────────────────────────────────────────────────────────
bool PenumbraInstructionSelector::selectGlobalValue(MachineInstr &I,
                                                     MachineBasicBlock &MBB,
                                                     MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  const GlobalValue *GV = I.getOperand(1).getGlobal();
  int64_t Offset = I.getOperand(1).getOffset();

  // TLS globals: the IR pass handles all TLS models.
  // GD/LD: replaced with __tls_get_addr call (this G_GLOBAL_VALUE is
  //        the argument — GOT address for PIC, TP offset for static).
  // LE/IE: replaced with read_tp + ptrtoint + add (this G_GLOBAL_VALUE
  //        provides the TP-relative offset via TLS relocations).
  if (GV->isThreadLocal()) {
    if (TM.getRelocationModel() == Reloc::PIC_) {
      // PIC TLS GD: compute GOT tls_index pair address via PC-relative
      // GOT offset.  The 4-instruction pattern (MOV PC + LLI/LUI + ADD)
      // plus the subsequent BL __tls_get_addr gives a 5-instruction
      // sequence that lld can relax to inline LE for static linking.
      DebugLoc DL = I.getDebugLoc();
      auto InsertPt = I.getIterator();
      Register PCReg =
          MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
      Register OffLoReg =
          MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
      Register OffReg =
          MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);

      auto MOVInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::MOV))
          .addDef(PCReg)
          .addReg(Penumbra::R15);
      constrainSelectedInstRegOperands(*MOVInst, TII, TRI, RBI);

      auto LLIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LLI))
          .addDef(OffLoReg)
          .add(MachineOperand::CreateGA(GV, Offset + 4,
                                        Penumbra::S_TLSgd_GOT_PCRel_Lo16));
      constrainSelectedInstRegOperands(*LLIInst, TII, TRI, RBI);

      auto LUIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LUI))
          .addDef(OffReg)
          .addReg(OffLoReg)
          .add(MachineOperand::CreateGA(GV, Offset + 8,
                                        Penumbra::S_TLSgd_GOT_PCRel_Hi16));
      constrainSelectedInstRegOperands(*LUIInst, TII, TRI, RBI);

      auto ADDInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADD))
          .addDef(DstReg)
          .addReg(PCReg)
          .addReg(OffReg);
      constrainSelectedInstRegOperands(*ADDInst, TII, TRI, RBI);
    } else {
      // Non-PIC TLS: absolute address with TLS GD relocs.
      // Used by LE path (PtrToInt) — linker resolves as R_TPREL.
      emitLoadSymbolAddr(
          DstReg, I.getDebugLoc(), MBB, I.getIterator(),
          MachineOperand::CreateGA(GV, Offset, Penumbra::S_TLSgd_Lo16),
          MachineOperand::CreateGA(GV, Offset, Penumbra::S_TLSgd_Hi16));
    }
    I.eraseFromParent();
    return true;
  }

  if (TM.getRelocationModel() == Reloc::PIC_) {
    // PIC/PIE: all globals go through the GOT.
    // GOT entries are full 32-bit data words — R_PENUMBRA_RELATIVE
    // patches them correctly for PIE (bootloader self-relocator).
    // No text relocs needed — code is PC-relative to the GOT entry.
    //
    //   MOV  PCReg, PC                          (capture PC = addr of MOV, P)
    //   LLI  OffReg, %got_pcrel_lo16(sym + 4)   (lo16(GOT[sym] - P))
    //   LUI  OffReg, %got_pcrel_hi16(sym + 8)   (hi16(GOT[sym] - P))
    //   ADD  PCReg, OffReg                       (PCReg = &GOT[sym])
    //   LDW  DstReg, [PCReg + 0]                (DstReg = *GOT[sym])
    //
    // Non-zero G_GLOBAL_VALUE offsets (e.g. &array[5]) are applied
    // after the GOT load via ADDi, since the GOT entry stores the
    // base symbol address only.
    DebugLoc DL = I.getDebugLoc();
    auto InsertPt = I.getIterator();
    Register PCReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
    Register OffLoReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
    Register OffReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
    Register GotAddrReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);

    // MOV PCReg, PC (R15) — capture address of this instruction
    auto MOVInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::MOV))
        .addDef(PCReg)
        .addReg(Penumbra::R15);
    constrainSelectedInstRegOperands(*MOVInst, TII, TRI, RBI);

    // LLI OffLoReg, %got_pcrel_lo16(sym + 4)
    auto LLIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LLI))
        .addDef(OffLoReg)
        .add(MachineOperand::CreateGA(GV, 4, Penumbra::S_GOT_PCRel_Lo16));
    constrainSelectedInstRegOperands(*LLIInst, TII, TRI, RBI);

    // LUI OffReg, OffLoReg, %got_pcrel_hi16(sym + 8)
    auto LUIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LUI))
        .addDef(OffReg)
        .addReg(OffLoReg)
        .add(MachineOperand::CreateGA(GV, 8, Penumbra::S_GOT_PCRel_Hi16));
    constrainSelectedInstRegOperands(*LUIInst, TII, TRI, RBI);

    // ADD GotAddrReg, PCReg, OffReg — PCReg + offset = &GOT[sym]
    auto ADDInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADD))
        .addDef(GotAddrReg)
        .addReg(PCReg)
        .addReg(OffReg);
    constrainSelectedInstRegOperands(*ADDInst, TII, TRI, RBI);

    // LDW DstReg/TmpReg, [GotAddrReg + 0] — load symbol address from GOT
    Register LoadDst = (Offset != 0)
        ? MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass)
        : DstReg;
    auto LDWInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LDW))
        .addDef(LoadDst)
        .addReg(GotAddrReg)
        .addImm(0);
    constrainSelectedInstRegOperands(*LDWInst, TII, TRI, RBI);

    // If G_GLOBAL_VALUE has a non-zero offset, add it after the GOT load.
    if (Offset != 0) {
      auto ADDiInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADDi))
          .addDef(DstReg)
          .addReg(LoadDst)
          .addImm(Offset);
      constrainSelectedInstRegOperands(*ADDiInst, TII, TRI, RBI);
    }
  } else {
    // Static: absolute address via LLI+LUI.
    emitLoadSymbolAddr(
        DstReg, I.getDebugLoc(), MBB, I.getIterator(),
        MachineOperand::CreateGA(GV, Offset, Penumbra::S_Lo16),
        MachineOperand::CreateGA(GV, Offset, Penumbra::S_Hi16));
  }

  I.eraseFromParent();
  return true;
}

// ── G_BLOCK_ADDR ─────────────────────────────────────────────────────────────
// Materialise a basic block address (GCC computed goto: &&label).
// Same as a global address — LLI+LUI (static) or MOV PC + ADDi (PIC).
bool PenumbraInstructionSelector::selectBlockAddress(MachineInstr &I,
                                                      MachineBasicBlock &MBB,
                                                      MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  const BlockAddress *BA = I.getOperand(1).getBlockAddress();
  int64_t Offset = I.getOperand(1).getOffset();

  if (TM.getRelocationModel() == Reloc::PIC_) {
    DebugLoc DL = I.getDebugLoc();
    auto InsertPt = I.getIterator();
    Register TmpReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
    auto MOVInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::MOV))
        .addDef(TmpReg)
        .addReg(Penumbra::R15);
    constrainSelectedInstRegOperands(*MOVInst, TII, TRI, RBI);
    auto ADDiInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADDi))
        .addDef(DstReg)
        .addReg(TmpReg)
        .add(MachineOperand::CreateBA(BA, Offset + 4, Penumbra::S_PCRel));
    constrainSelectedInstRegOperands(*ADDiInst, TII, TRI, RBI);
  } else {
    emitLoadSymbolAddr(
        DstReg, I.getDebugLoc(), MBB, I.getIterator(),
        MachineOperand::CreateBA(BA, Offset, Penumbra::S_Lo16),
        MachineOperand::CreateBA(BA, Offset, Penumbra::S_Hi16));
  }

  I.eraseFromParent();
  return true;
}

// ── G_JUMP_TABLE ─────────────────────────────────────────────────────────────
bool PenumbraInstructionSelector::selectJumpTable(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  unsigned JTI = I.getOperand(1).getIndex();

  if (TM.getRelocationModel() == Reloc::PIC_) {
    // PIC: materialise JT base as PC + pcrel offset.
    DebugLoc DL = I.getDebugLoc();
    auto InsertPt = I.getIterator();
    Register TmpReg =
        MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);

    auto MOVInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::MOV))
        .addDef(TmpReg)
        .addReg(Penumbra::R15);
    constrainSelectedInstRegOperands(*MOVInst, TII, TRI, RBI);

    auto ADDiInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADDi))
        .addDef(DstReg)
        .addReg(TmpReg)
        .add(MachineOperand::CreateJTI(JTI, Penumbra::S_PCRel));
    constrainSelectedInstRegOperands(*ADDiInst, TII, TRI, RBI);
  } else {
    // Static: absolute address via LLI+LUI.
    emitLoadSymbolAddr(
        DstReg, I.getDebugLoc(), MBB, I.getIterator(),
        MachineOperand::CreateJTI(JTI, Penumbra::S_Lo16),
        MachineOperand::CreateJTI(JTI, Penumbra::S_Hi16));
  }

  I.eraseFromParent();
  return true;
}

// ── G_BRJT (indexed jump through table) ──────────────────────────────────────
// Always label-difference entries:
//   SHLi idx,2 → ADD idx,base → LDW offset,[idx] → ADD offset,base → JMP offset
bool PenumbraInstructionSelector::selectBrJT(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register BaseReg = I.getOperand(0).getReg();
  // operand 1 is the JTI metadata — not needed at this stage
  Register IdxReg = I.getOperand(2).getReg();
  const DebugLoc &DL = I.getDebugLoc();
  // Always label-difference entries — always add base back.
  (void)TM;

  // tmp = index << 2 (word-sized entries)
  Register ShiftReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  auto SHLInst = BuildMI(MBB, I, DL, TII.get(Penumbra::SHLi))
      .addDef(ShiftReg)
      .addReg(IdxReg)
      .addImm(2);
  constrainSelectedInstRegOperands(*SHLInst, TII, TRI, RBI);

  // tmp = tmp + base
  Register AddrReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  auto AddInst = BuildMI(MBB, I, DL, TII.get(Penumbra::ADD))
      .addDef(AddrReg)
      .addReg(ShiftReg)
      .addReg(BaseReg);
  constrainSelectedInstRegOperands(*AddInst, TII, TRI, RBI);

  // entry = [tmp + 0]
  Register EntryReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  auto LDWInst = BuildMI(MBB, I, DL, TII.get(Penumbra::LDW))
      .addDef(EntryReg)
      .addReg(AddrReg)
      .addImm(0);
  constrainSelectedInstRegOperands(*LDWInst, TII, TRI, RBI);

  // Entry is a label difference (target - JT_base).
  // Add JT base back to get the absolute target.
  Register TargetReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  auto AddBackInst = BuildMI(MBB, I, DL, TII.get(Penumbra::ADD))
      .addDef(TargetReg)
      .addReg(EntryReg)
      .addReg(BaseReg);
  constrainSelectedInstRegOperands(*AddBackInst, TII, TRI, RBI);

  // Indirect branch (not a return — targets are within this function)
  auto BRInst = BuildMI(MBB, I, DL, TII.get(Penumbra::BRIND))
      .addReg(TargetReg);
  constrainSelectedInstRegOperands(*BRInst, TII, TRI, RBI);

  I.eraseFromParent();
  return true;
}

// ── G_INTRINSIC ─────────────────────────────────────────────────────────────
bool PenumbraInstructionSelector::selectIntrinsic(
    MachineInstr &I, MachineBasicBlock &MBB, MachineRegisterInfo &MRI) const {
  unsigned IntrinID = cast<GIntrinsic>(I).getIntrinsicID();
  const DebugLoc &DL = I.getDebugLoc();

  // __builtin_return_address(0) → read a vreg that captures R13 at
  // function entry.  Reading R13 directly would be wrong in non-leaf
  // functions: every BL/JALR clobbers R13 with its own call-site
  // return address before we reach this point.  The capture is done
  // lazily: on first use, insert a MOV from R13 into a fresh vreg at
  // the top of the entry block; subsequent uses reuse that vreg.
  if (IntrinID == Intrinsic::returnaddress) {
    Register DstReg = I.getOperand(0).getReg();
    unsigned Depth = I.getOperand(2).getImm();
    // Depth > 0 would need a frame-pointer chain to walk, which
    // Penumbra's ABI doesn't provide.  The GCC/Clang contract
    // permits returning an unspecified value in that case, so
    // materialize 0 — callers that check the result stay correct,
    // and compilation succeeds rather than failing selection.
    if (Depth != 0) {
      auto NewI = BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
          .addDef(DstReg)
          .addImm(0);
      I.eraseFromParent();
      return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
    }
    MachineFunction &MF = *I.getParent()->getParent();
    MF.getFrameInfo().setReturnAddressIsTaken(true);
    auto *FuncInfo = MF.getInfo<PenumbraMachineFunctionInfo>();
    Register RAVReg = FuncInfo->getReturnAddressVReg();
    if (!RAVReg.isValid()) {
      // First reference: materialize the capture at entry-block head.
      // Both the function-level MRI and the entry block need R13 marked
      // as live-in.  The MRI live-in is what `spillCalleeSavedRegisters`
      // checks to suppress the kill flag on R13's CSR spill so that this
      // later read stays valid.
      MachineBasicBlock &EntryMBB = MF.front();
      if (!MRI.isLiveIn(Penumbra::R13))
        MRI.addLiveIn(Penumbra::R13);
      if (!EntryMBB.isLiveIn(Penumbra::R13))
        EntryMBB.addLiveIn(Penumbra::R13);
      RAVReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
      BuildMI(EntryMBB, EntryMBB.begin(), DL, TII.get(Penumbra::MOV))
          .addDef(RAVReg)
          .addReg(Penumbra::R13);
      FuncInfo->setReturnAddressVReg(RAVReg);
    }
    auto NewI = BuildMI(MBB, I, DL, TII.get(Penumbra::MOV))
        .addDef(DstReg)
        .addReg(RAVReg);
    I.eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // __builtin_frame_address(0) → read SP (R14).
  if (IntrinID == Intrinsic::frameaddress) {
    Register DstReg = I.getOperand(0).getReg();
    unsigned Depth = I.getOperand(2).getImm();
    if (Depth != 0)
      return false;
    if (!MBB.isLiveIn(Penumbra::R14))
      MBB.addLiveIn(Penumbra::R14);
    auto NewI = BuildMI(MBB, I, DL, TII.get(Penumbra::MOV))
        .addDef(DstReg)
        .addReg(Penumbra::R14);
    I.eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // va_copy(dst, src) — copy the va_list pointer.
  // On Penumbra, va_list is just a pointer (4 bytes), so this is a
  // 4-byte store: *dst = *src (both operands are pointers to va_list).
  if (IntrinID == Intrinsic::vacopy) {
    Register DstPtr = I.getOperand(1).getReg();
    Register SrcPtr = I.getOperand(2).getReg();
    // Load the pointer value from src va_list
    Register Tmp = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
    BuildMI(MBB, I, DL, TII.get(Penumbra::LDW))
        .addDef(Tmp)
        .addReg(SrcPtr)
        .addImm(0);
    // Store it to dst va_list
    BuildMI(MBB, I, DL, TII.get(Penumbra::STW))
        .addReg(Tmp)
        .addReg(DstPtr)
        .addImm(0);
    I.eraseFromParent();
    return true;
  }

  // __clear_cache(begin, end) — flush I-cache for modified code pages.
  // TODO: Penumbra has split I/D caches and needs a real icache flush
  // syscall for hardware.  On the ISS there's no separate I-cache, so
  // a no-op is correct.  When the kernel gains an icache flush syscall,
  // emit a call to __clear_cache here instead.
  if (IntrinID == Intrinsic::clear_cache) {
    I.eraseFromParent();
    return true;
  }

  return false;
}

// ── Factory function ──────────────────────────────────────────────────────────

namespace llvm {
InstructionSelector *
createPenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                                  const PenumbraSubtarget &STI,
                                  const PenumbraRegisterBankInfo &RBI) {
  return new PenumbraInstructionSelector(TM, STI, RBI);
}
} // namespace llvm

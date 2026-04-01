//===-- PenumbraInstructionSelector.cpp - Penumbra Instruction Selector ---===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PenumbraFixupKinds.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraRegisterBankInfo.h"
#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGenTypes/LowLevelType.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "penumbra-isel"

using namespace llvm;

namespace {

class PenumbraInstructionSelector : public InstructionSelector {
public:
  PenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                               const PenumbraSubtarget &STI,
                               const PenumbraRegisterBankInfo &RBI);

  bool select(MachineInstr &I) override;
  static const char *getName() { return DEBUG_TYPE; }

  // No TableGen-generated match table yet — per-function state is a no-op.
  void setupGeneratedPerFunctionState(MachineFunction &) override {}

private:
  bool selectBinaryALU(MachineInstr &I, MachineBasicBlock &MBB,
                       MachineRegisterInfo &MRI, unsigned Opc) const;
  bool selectShift(MachineInstr &I, MachineBasicBlock &MBB,
                   MachineRegisterInfo &MRI, unsigned RegOpc,
                   unsigned ImmOpc) const;
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
  bool selectJumpTable(MachineInstr &I, MachineBasicBlock &MBB,
                       MachineRegisterInfo &MRI) const;
  bool selectBrJT(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;

  // Emit LLI+LUI pair to materialise a symbol address into DstReg.
  // LoOp/HiOp are the lo16/hi16 operands (GlobalAddress, JumpTableIndex, etc.)
  void emitLoadSymbolAddr(Register DstReg, const DebugLoc &DL,
                          MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator InsertPt,
                          const MachineOperand &LoOp,
                          const MachineOperand &HiOp) const;

  const PenumbraInstrInfo &TII;
  const PenumbraRegisterInfo &TRI;
  const PenumbraRegisterBankInfo &RBI;
};

} // end anonymous namespace

PenumbraInstructionSelector::PenumbraInstructionSelector(
    const PenumbraTargetMachine &TM, const PenumbraSubtarget &STI,
    const PenumbraRegisterBankInfo &RBI)
    : InstructionSelector(), TII(*STI.getInstrInfo()),
      TRI(*STI.getRegisterInfo()), RBI(RBI) {}

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

  switch (I.getOpcode()) {
  // ── Binary ALU ────────────────────────────────────────────────────────────
  // All map to 3-operand machine instructions with a tied dest=src1 constraint.
  // The register allocator inserts a COPY to satisfy the tie in SSA form.
  case G_ADD:  return selectBinaryALU(I, MBB, MRI, Penumbra::ADD);
  case G_SUB:  return selectBinaryALU(I, MBB, MRI, Penumbra::SUB);
  case G_AND:  return selectBinaryALU(I, MBB, MRI, Penumbra::AND);
  case G_OR:   return selectBinaryALU(I, MBB, MRI, Penumbra::OR);
  case G_XOR:  return selectBinaryALU(I, MBB, MRI, Penumbra::XOR);
  case G_SHL:  return selectShift(I, MBB, MRI, Penumbra::SHL, Penumbra::SHLi);
  case G_LSHR: return selectShift(I, MBB, MRI, Penumbra::SHR, Penumbra::SHRi);
  case G_ASHR: return selectShift(I, MBB, MRI, Penumbra::SAR, Penumbra::SARi);

  // ── Constants / Addresses ─────────────────────────────────────────────────
  case G_CONSTANT:     return selectConstant(I, MBB, MRI);
  case G_GLOBAL_VALUE: return selectGlobalValue(I, MBB, MRI);

  // ── Pointer arithmetic ────────────────────────────────────────────────────
  case G_PTR_ADD: return selectBinaryALU(I, MBB, MRI, Penumbra::ADD);

  // ── Memory ────────────────────────────────────────────────────────────────
  case G_LOAD:        return selectLoad(I, MBB, MRI);
  case G_STORE:       return selectStore(I, MBB, MRI);
  case G_FRAME_INDEX: return selectFrameIndex(I, MBB, MRI);
  case G_JUMP_TABLE:  return selectJumpTable(I, MBB, MRI);
  case G_BRJT:        return selectBrJT(I, MBB, MRI);

  // ── Branches ──────────────────────────────────────────────────────────────
  case G_BR:     return selectBranch(I, MBB);
  case G_BRCOND: return selectBrCond(I, MBB, MRI);

  // ── Extensions / Truncation ─────────────────────────────────────────────────
  case G_ANYEXT:
  case G_TRUNC:
  case G_INTTOPTR:
  case G_PTRTOINT:
    I.setDesc(TII.get(TargetOpcode::COPY));
    return RBI.constrainGenericRegister(
        I.getOperand(0).getReg(), Penumbra::GPR_AllocatableRegClass, MRI);

  case G_ZEXT: return selectZExt(I, MBB, MRI);
  case G_SEXT: return selectSExt(I, MBB, MRI);

  // ── Compare / Select ────────────────────────────────────────────────────────
  case G_ICMP:   return selectICmp(I, MBB, MRI);
  case G_SELECT: return selectSelect(I, MBB, MRI);

  default:
    return false;
  }
}

// ── Binary ALU helper ─────────────────────────────────────────────────────────
// G_ADD/G_SUB/... have operands: [def dst, use src1, use src2]
// Penumbra ALU has:              [def $Rd, use $Rd_in(tied), use $Rs]
// We emit them directly; the RA satisfies the tied constraint by copying src1.
bool PenumbraInstructionSelector::selectBinaryALU(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI,
                                                   unsigned Opc) const {
  MachineInstr *NewI =
      BuildMI(MBB, I, I.getDebugLoc(), TII.get(Opc))
          .addDef(I.getOperand(0).getReg())  // $Rd  (output)
          .addReg(I.getOperand(1).getReg())  // $Rd_in (tied; RA inserts COPY)
          .addReg(I.getOperand(2).getReg()); // $Rs
  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── Shift helper ─────────────────────────────────────────────────────────────
// Like selectBinaryALU, but folds a constant shift amount into the immediate
// form (SHLi/SHRi/SARi) when the second operand is a G_CONSTANT in 0–31.
bool PenumbraInstructionSelector::selectShift(MachineInstr &I,
                                               MachineBasicBlock &MBB,
                                               MachineRegisterInfo &MRI,
                                               unsigned RegOpc,
                                               unsigned ImmOpc) const {
  Register ShAmtReg = I.getOperand(2).getReg();
  MachineInstr *ShAmtDef = MRI.getVRegDef(ShAmtReg);

  if (ShAmtDef && ShAmtDef->getOpcode() == TargetOpcode::G_CONSTANT) {
    int64_t Amt = ShAmtDef->getOperand(1).getCImm()->getSExtValue();
    MachineInstr *NewI =
        BuildMI(MBB, I, I.getDebugLoc(), TII.get(ImmOpc))
            .addDef(I.getOperand(0).getReg())
            .addReg(I.getOperand(1).getReg())
            .addImm(Amt);
    I.eraseFromParent();
    if (MRI.use_nodbg_empty(ShAmtDef->getOperand(0).getReg()))
      ShAmtDef->eraseFromParent();
    return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
  }

  // Non-constant or out-of-range: fall back to register shift.
  return selectBinaryALU(I, MBB, MRI, RegOpc);
}

// ── G_CONSTANT ────────────────────────────────────────────────────────────────
// Materialize a 32-bit constant into a register.
// Penumbra immediate instructions:
//   LLI  Rd, #imm16 — load zero-extended 16-bit value into Rd
//   LLIS Rd, #imm16 — load sign-extended 16-bit value into Rd
//   LUI  Rd, #imm16 — load upper 16 bits: Rd = (Rd & 0xFFFF) | (imm16 << 16)
//                     (tied: $Rd = $Rd_in — Rd is both input and output)
bool PenumbraInstructionSelector::selectConstant(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  int64_t Val = I.getOperand(1).getCImm()->getSExtValue();
  const DebugLoc &DL = I.getDebugLoc();

  if (Val >= 0 && Val <= 0xFFFF) {
    auto MI = BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
        .addDef(DstReg)
        .addImm(Val);
    constrainSelectedInstRegOperands(*MI, TII, TRI, RBI);
  } else if (Val < 0 && Val >= -32768) {
    auto MI = BuildMI(MBB, I, DL, TII.get(Penumbra::LLIS))
        .addDef(DstReg)
        .addImm(Val);
    constrainSelectedInstRegOperands(*MI, TII, TRI, RBI);
  } else {
    auto MI1 = BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
        .addDef(DstReg)
        .addImm(Val & 0xFFFF);
    constrainSelectedInstRegOperands(*MI1, TII, TRI, RBI);
    auto MI2 = BuildMI(MBB, I, DL, TII.get(Penumbra::LUI))
        .addDef(DstReg)
        .addReg(DstReg)
        .addImm((Val >> 16) & 0xFFFF);
    constrainSelectedInstRegOperands(*MI2, TII, TRI, RBI);
  }

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

    unsigned BrOpc = icmpPredToBranchOpc(Pred);
    if (!BrOpc)
      return false;

    auto CmpMI = BuildMI(MBB, I, DL, TII.get(Penumbra::CMP))
                     .addReg(LHS)
                     .addReg(RHS);
    constrainSelectedInstRegOperands(*CmpMI, TII, TRI, RBI);

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
  auto LLIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LLI))
      .addDef(DstReg)
      .add(LoOp);
  constrainSelectedInstRegOperands(*LLIInst, TII, TRI, RBI);

  auto LUIInst = BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LUI))
      .addDef(DstReg)
      .addReg(DstReg)
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

  emitLoadSymbolAddr(
      DstReg, I.getDebugLoc(), MBB, I.getIterator(),
      MachineOperand::CreateGA(GV, Offset, Penumbra::S_Lo16),
      MachineOperand::CreateGA(GV, Offset, Penumbra::S_Hi16));

  I.eraseFromParent();
  return true;
}

// ── G_JUMP_TABLE ─────────────────────────────────────────────────────────────
bool PenumbraInstructionSelector::selectJumpTable(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  unsigned JTI = I.getOperand(1).getIndex();

  emitLoadSymbolAddr(
      DstReg, I.getDebugLoc(), MBB, I.getIterator(),
      MachineOperand::CreateJTI(JTI, Penumbra::S_Lo16),
      MachineOperand::CreateJTI(JTI, Penumbra::S_Hi16));

  I.eraseFromParent();
  return true;
}

// ── G_BRJT (indexed jump through table) ──────────────────────────────────────
// Expands to: SHLi tmp, index, #2  →  ADD tmp, base  →  LDW target, [tmp]
//             →  JMP target
bool PenumbraInstructionSelector::selectBrJT(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register BaseReg = I.getOperand(0).getReg();
  // operand 1 is the JTI metadata — not needed at this stage
  Register IdxReg = I.getOperand(2).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  // tmp = index << 2 (word-sized entries)
  Register ShiftReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, I, DL, TII.get(Penumbra::SHLi))
      .addDef(ShiftReg)
      .addReg(IdxReg)
      .addImm(2);

  // tmp = tmp + base
  Register AddrReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, I, DL, TII.get(Penumbra::ADD))
      .addDef(AddrReg)
      .addReg(ShiftReg)
      .addReg(BaseReg);

  // target = [tmp + 0]
  Register TargetReg = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, I, DL, TII.get(Penumbra::LDW))
      .addDef(TargetReg)
      .addReg(AddrReg)
      .addImm(0);

  // Indirect branch (not a return — targets are within this function)
  BuildMI(MBB, I, DL, TII.get(Penumbra::BRIND))
      .addReg(TargetReg);

  I.eraseFromParent();
  return true;
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

//===-- PenumbraISelLowering.cpp - Penumbra DAG Lowering -------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraISelLowering.h"
#include "PenumbraInstrInfo.h"
#include "PenumbraRegisterInfo.h"
#include "PenumbraSubtarget.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/TargetLowering.h"

using namespace llvm;

// TableGen-generated CC assignment functions (CC_Penumbra, RetCC_Penumbra).
// Must be included after `using namespace llvm` — the generated code uses
// MVT, CCValAssign, CCState etc. without namespace qualification.
#include "PenumbraGenCallingConv.inc"

PenumbraISelLowering::PenumbraISelLowering(const TargetMachine &TM,
                                            const PenumbraSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &Penumbra::GPR_AllocatableRegClass);

  // SP register — needed by G_DYN_STACKALLOC lowering and G_STACKSAVE/RESTORE.
  setStackPointerRegisterToSaveRestore(Penumbra::R14);

  // No hardware atomics — all atomic operations expand to __atomic_* libcalls
  // via AtomicExpandPass.  The library handles synchronization (interrupt-
  // disable CAS now, RAS or LL/SC in future).
  setMaxAtomicSizeInBitsSupported(0);

  // No multiply/divide hardware — expand to libcalls
  setOperationAction(ISD::MUL,        MVT::i32, Expand);
  setOperationAction(ISD::MULHS,      MVT::i32, Expand);
  setOperationAction(ISD::MULHU,      MVT::i32, Expand);
  setOperationAction(ISD::SMUL_LOHI,  MVT::i32, Expand);
  setOperationAction(ISD::UMUL_LOHI,  MVT::i32, Expand);
  setOperationAction(ISD::SDIV,       MVT::i32, Expand);
  setOperationAction(ISD::UDIV,       MVT::i32, Expand);
  setOperationAction(ISD::SREM,       MVT::i32, Expand);
  setOperationAction(ISD::UREM,       MVT::i32, Expand);
  setOperationAction(ISD::SDIVREM,    MVT::i32, Expand);
  setOperationAction(ISD::UDIVREM,    MVT::i32, Expand);

  // No bit-manipulation instructions
  setOperationAction(ISD::BSWAP,   MVT::i32, Expand);
  setOperationAction(ISD::ROTL,    MVT::i32, Expand);
  setOperationAction(ISD::ROTR,    MVT::i32, Expand);
  setOperationAction(ISD::CTLZ,    MVT::i32, Expand);
  setOperationAction(ISD::CTTZ,    MVT::i32, Expand);
  setOperationAction(ISD::CTPOP,   MVT::i32, Expand);

  computeRegisterProperties(STI.getRegisterInfo());
}

unsigned PenumbraISelLowering::getJumpTableEncoding() const {
  // Always use label-difference entries (target - JT_base).  This avoids
  // dynamic relocations, works at any load address (static, PIC, PIE),
  // and enables future optimization to 16-bit entries when offsets fit
  // in ±32KB.  The instruction selector adds the JT base back at runtime.
  return MachineJumpTableInfo::EK_LabelDifference32;
}

CCAssignFn *PenumbraISelLowering::getCCAssignFn(CallingConv::ID CC,
                                                 bool Return,
                                                 bool IsVarArg) const {
  return Return ? RetCC_Penumbra : CC_Penumbra;
}

//===----------------------------------------------------------------------===//
// Exception handling
//===----------------------------------------------------------------------===//

Register PenumbraISelLowering::getExceptionPointerRegister(
    const Constant *PersonalityFn) const {
  return Penumbra::R1;
}

Register PenumbraISelLowering::getExceptionSelectorRegister(
    const Constant *PersonalityFn) const {
  return Penumbra::R2;
}

//===----------------------------------------------------------------------===//
// Inline assembly support
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
PenumbraISelLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return C_RegisterClass;
    default:
      break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
PenumbraISelLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      if (VT == MVT::i32 || VT == MVT::Other)
        return {0U, &Penumbra::GPR_AllocatableRegClass};
      break;
    }
  }

  // Physical register name in braces: {r1}, {sp}, {cc}, etc.
  if (StringRef(Constraint).starts_with("{") &&
      StringRef(Constraint).ends_with("}")) {
    StringRef RegName = Constraint.substr(1, Constraint.size() - 2);
    // Map "cc" to the status register (condition code clobber).
    if (RegName == "cc")
      return {Penumbra::SR, &Penumbra::CCRRegClass};
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

// Expand SELECT_GPR / SELECT_CC_GPR pseudo into a conditional-branch diamond.
//
// After expansion (three blocks):
//   thisMBB:
//     ... preceding instructions ...
//     TEST/CMP (set flags)
//     Bcc tailMBB           ; condition true → trueval wins
//   falseMBB:               ; fall-through (condition false)
//     B tailMBB
//   tailMBB:
//     %dst = PHI(%trueval, thisMBB, %falseval, falseMBB)
//     ... following instructions ...
MachineBasicBlock *
PenumbraISelLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                   MachineBasicBlock *MBB) const {
  unsigned Opc = MI.getOpcode();
  assert((Opc == Penumbra::SELECT_GPR || Opc == Penumbra::SELECT_CC_GPR) &&
         "Unexpected custom inserter instruction");

  MachineFunction *MF = MBB->getParent();
  const auto &TII = *MF->getSubtarget().getInstrInfo();
  const auto *BB = MBB->getBasicBlock();
  const auto DL = MI.getDebugLoc();

  auto I = ++MBB->getIterator();

  auto *FalseMBB = MF->CreateMachineBasicBlock(BB);
  auto *TailMBB = MF->CreateMachineBasicBlock(BB);
  MF->insert(I, FalseMBB);
  MF->insert(I, TailMBB);

  FalseMBB->setCallFrameSize(MBB->getCallFrameSize());
  TailMBB->setCallFrameSize(MBB->getCallFrameSize());

  // Move tail instructions and transfer original successors BEFORE adding
  // new CFG edges (transferSuccessors moves ALL successors from MBB).
  TailMBB->splice(TailMBB->end(), MBB, std::next(MI.getIterator()), MBB->end());
  TailMBB->transferSuccessorsAndUpdatePHIs(MBB);

  MBB->addSuccessor(FalseMBB);
  MBB->addSuccessor(TailMBB);
  FalseMBB->addSuccessor(TailMBB);

  // PHI in tailMBB picks trueval or falseval depending on which path was taken.
  BuildMI(*TailMBB, TailMBB->begin(), DL, TII.get(Penumbra::PHI),
          MI.getOperand(0).getReg())
      .addReg(MI.getOperand(1).getReg())
      .addMBB(MBB)
      .addReg(MI.getOperand(2).getReg())
      .addMBB(FalseMBB);

  // Emit the flag-setting instruction + conditional branch in thisMBB.
  if (Opc == Penumbra::SELECT_CC_GPR) {
    // SELECT_CC_GPR: operands are dst, trueval, falseval, lhs, rhs, cc.
    BuildMI(MBB, DL, TII.get(Penumbra::CMP))
        .addReg(MI.getOperand(3).getReg())
        .addReg(MI.getOperand(4).getReg());
    unsigned BrOpc = MI.getOperand(5).getImm();
    BuildMI(MBB, DL, TII.get(BrOpc)).addMBB(TailMBB);
  } else {
    // SELECT_GPR: operands are dst, trueval, falseval, cond.
    auto CondReg = MI.getOperand(3).getReg();
    BuildMI(MBB, DL, TII.get(Penumbra::TEST))
        .addReg(CondReg)
        .addReg(CondReg);
    BuildMI(MBB, DL, TII.get(Penumbra::BNE)).addMBB(TailMBB);
  }

  // FalseMBB: unconditional jump to tailMBB.
  BuildMI(FalseMBB, DL, TII.get(Penumbra::B)).addMBB(TailMBB);

  MI.eraseFromParent();
  return TailMBB;
}

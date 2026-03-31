//===-- PenumbraRegisterInfo.cpp - Penumbra Register Info ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraRegisterInfo.h"
#include "PenumbraFrameLowering.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#define GET_REGINFO_TARGET_DESC
#include "PenumbraGenRegisterInfo.inc"

using namespace llvm;

PenumbraRegisterInfo::PenumbraRegisterInfo()
    : PenumbraGenRegisterInfo(/*ReturnAddress=*/Penumbra::R13,
                              /*MiscTempReg=*/0,
                              /*IntRetAddrReg=*/0,
                              /*PCReg=*/Penumbra::R15) {}

const MCPhysReg *
PenumbraRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_Penumbra_SaveList;
}

const uint32_t *
PenumbraRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                           CallingConv::ID CC) const {
  return CSR_Penumbra_RegMask;
}

BitVector
PenumbraRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  Reserved.set(Penumbra::R0);  // Hardwired zero
  Reserved.set(Penumbra::R12); // Thread pointer
  Reserved.set(Penumbra::R14); // Stack pointer
  Reserved.set(Penumbra::R15); // Program counter
  return Reserved;
}

bool PenumbraRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                               int SPAdj,
                                               unsigned FIOperandNum,
                                               RegScavenger *RS) const {
  MachineInstr &Inst = *MI;
  MachineFunction &MF = *Inst.getParent()->getParent();
  const MachineFrameInfo &MFI = MF.getFrameInfo();

  // Calculate the actual offset: frame object offset + SP adjustment.
  int Offset = MFI.getObjectOffset(Inst.getOperand(FIOperandNum).getIndex()) +
               MFI.getStackSize() + SPAdj;

  // Replace the frame index with the stack pointer and set the offset.
  Inst.getOperand(FIOperandNum).ChangeToRegister(Penumbra::R14, /*isDef=*/false);
  // The offset operand is typically the next one (FIOperandNum + 1).
  Inst.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);

  return false;
}

Register
PenumbraRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return Penumbra::R14; // SP — no frame pointer by default
}

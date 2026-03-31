//===-- PenumbraInstrInfo.cpp - Penumbra Instruction Info ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraInstrInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "PenumbraGenInstrInfo.inc"

using namespace llvm;

PenumbraInstrInfo::PenumbraInstrInfo(const TargetSubtargetInfo &STI,
                                     const TargetRegisterInfo &TRI)
    : PenumbraGenInstrInfo(STI, TRI, Penumbra::ADJCALLSTACKDOWN,
                           Penumbra::ADJCALLSTACKUP, ~0u, Penumbra::RET) {}

void PenumbraInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MI,
                                     const DebugLoc &DL, Register DestReg,
                                     Register SrcReg, bool KillSrc,
                                     bool RenamableDest,
                                     bool RenamableSrc) const {
  BuildMI(MBB, MI, DL, get(Penumbra::MOV), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

void PenumbraInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool IsKill, int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOStore,
      MFI.getObjectSize(FrameIdx), MFI.getObjectAlign(FrameIdx));

  BuildMI(MBB, MI, DebugLoc(), get(Penumbra::STW))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FrameIdx)
      .addImm(0)
      .addMemOperand(MMO);
}

void PenumbraInstrInfo::loadRegFromStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
    int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    unsigned SubReg, MachineInstr::MIFlag Flags) const {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOLoad,
      MFI.getObjectSize(FrameIdx), MFI.getObjectAlign(FrameIdx));

  BuildMI(MBB, MI, DebugLoc(), get(Penumbra::LDW), DestReg)
      .addFrameIndex(FrameIdx)
      .addImm(0)
      .addMemOperand(MMO);
}

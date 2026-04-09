//===-- PenumbraInstrInfo.cpp - Penumbra Instruction Info ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraInstrInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

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

bool PenumbraInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  if (MI.getOpcode() != Penumbra::LEAfi)
    return false;

  // LEAfi Rd, R14, #offset  →  MOV Rd, R14  [+ ADDi Rd, #offset if nonzero]
  MachineBasicBlock &MBB = *MI.getParent();
  const DebugLoc &DL = MI.getDebugLoc();
  Register DstReg = MI.getOperand(0).getReg();
  Register BaseReg = MI.getOperand(1).getReg();
  int64_t Offset = MI.getOperand(2).getImm();

  BuildMI(MBB, MI, DL, get(Penumbra::MOV), DstReg)
      .addReg(BaseReg);

  if (Offset != 0) {
    BuildMI(MBB, MI, DL, get(Penumbra::ADDi), DstReg)
        .addReg(DstReg)
        .addImm(Offset);
  }

  MI.eraseFromParent();
  return true;
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

// ===----------------------------------------------------------------------===//
// Branch analysis / insertion / removal
// ===----------------------------------------------------------------------===//

static bool isCondBranch(unsigned Opc) {
  switch (Opc) {
  case Penumbra::BEQ: case Penumbra::BNE:
  case Penumbra::BCS: case Penumbra::BCC:
  case Penumbra::BMI: case Penumbra::BPL:
  case Penumbra::BVS: case Penumbra::BVC:
  case Penumbra::BHI: case Penumbra::BLS:
  case Penumbra::BGE: case Penumbra::BLT:
  case Penumbra::BGT: case Penumbra::BLE:
    return true;
  default:
    return false;
  }
}

static bool isUncondBranch(unsigned Opc) {
  return Opc == Penumbra::B;
}

unsigned PenumbraInstrInfo::getOppositeBranchOpcode(unsigned Opc) {
  switch (Opc) {
  case Penumbra::BEQ: return Penumbra::BNE;
  case Penumbra::BNE: return Penumbra::BEQ;
  case Penumbra::BCS: return Penumbra::BCC;
  case Penumbra::BCC: return Penumbra::BCS;
  case Penumbra::BMI: return Penumbra::BPL;
  case Penumbra::BPL: return Penumbra::BMI;
  case Penumbra::BVS: return Penumbra::BVC;
  case Penumbra::BVC: return Penumbra::BVS;
  case Penumbra::BHI: return Penumbra::BLS;
  case Penumbra::BLS: return Penumbra::BHI;
  case Penumbra::BGE: return Penumbra::BLT;
  case Penumbra::BLT: return Penumbra::BGE;
  case Penumbra::BGT: return Penumbra::BLE;
  case Penumbra::BLE: return Penumbra::BGT;
  default: llvm_unreachable("not a conditional branch");
  }
}

bool PenumbraInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *&TBB,
                                       MachineBasicBlock *&FBB,
                                       SmallVectorImpl<MachineOperand> &Cond,
                                       bool AllowModify) const {
  TBB = nullptr;
  FBB = nullptr;
  Cond.clear();

  // Scan backwards past non-terminators and debug values.
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!I->isTerminator())
      break;
  }
  // Now advance past the non-terminator to the first terminator.
  if (I != MBB.end() && !I->isTerminator())
    ++I;

  // No terminators — block falls through.
  if (I == MBB.end())
    return false;

  MachineInstr &FirstTerm = *I;
  unsigned FirstOpc = FirstTerm.getOpcode();

  // Unanalyzable terminators (indirect branches, returns, etc.).
  if (!isCondBranch(FirstOpc) && !isUncondBranch(FirstOpc))
    return true;

  // Check for a second terminator.
  MachineInstr *SecondTerm = nullptr;
  MachineBasicBlock::iterator Next = std::next(I);
  if (Next != MBB.end() && Next->isTerminator()) {
    SecondTerm = &*Next;
    // More than two terminators — unanalyzable.
    MachineBasicBlock::iterator Third = std::next(Next);
    if (Third != MBB.end() && Third->isTerminator())
      return true;
  }

  // Single unconditional branch.
  if (isUncondBranch(FirstOpc) && !SecondTerm) {
    TBB = FirstTerm.getOperand(0).getMBB();
    return false;
  }

  // Single conditional branch (fallthrough on false).
  if (isCondBranch(FirstOpc) && !SecondTerm) {
    TBB = FirstTerm.getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(FirstOpc));
    return false;
  }

  // Conditional followed by unconditional.
  if (isCondBranch(FirstOpc) && SecondTerm &&
      isUncondBranch(SecondTerm->getOpcode())) {
    TBB = FirstTerm.getOperand(0).getMBB();
    FBB = SecondTerm->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(FirstOpc));
    return false;
  }

  // Anything else is unanalyzable.
  return true;
}

unsigned PenumbraInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                          MachineBasicBlock *TBB,
                                          MachineBasicBlock *FBB,
                                          ArrayRef<MachineOperand> Cond,
                                          const DebugLoc &DL,
                                          int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  unsigned Count = 0;

  if (Cond.empty()) {
    // Unconditional branch.
    BuildMI(&MBB, DL, get(Penumbra::B)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded = 4;
    return 1;
  }

  // Conditional branch.
  assert(Cond.size() == 1 && "Penumbra branch condition has one element");
  unsigned Opc = Cond[0].getImm();
  BuildMI(&MBB, DL, get(Opc)).addMBB(TBB);
  Count = 1;
  int Bytes = 4;

  if (FBB) {
    // Two-way: conditional to TBB, unconditional to FBB.
    BuildMI(&MBB, DL, get(Penumbra::B)).addMBB(FBB);
    Count = 2;
    Bytes += 4;
  }

  if (BytesAdded)
    *BytesAdded = Bytes;
  return Count;
}

unsigned PenumbraInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                          int *BytesRemoved) const {
  unsigned Count = 0;
  int Bytes = 0;

  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    unsigned Opc = I->getOpcode();
    if (!isCondBranch(Opc) && !isUncondBranch(Opc))
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
    Bytes += 4;
  }

  if (BytesRemoved)
    *BytesRemoved = Bytes;
  return Count;
}

bool PenumbraInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Penumbra branch condition has one element");
  Cond[0].setImm(getOppositeBranchOpcode(Cond[0].getImm()));
  return false;
}

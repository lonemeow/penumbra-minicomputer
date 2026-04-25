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

//===----------------------------------------------------------------------===//
// Compare-elimination peephole.
//
// Penumbra has flag-setting ALU instructions (Defs = [SR]) and a separate
// CMP/CMPi/TEST/TESTi family that *only* sets flags.  After GISel folds
// G_ICMP+G_BRCOND into CMPi+Bcc, code sequences like
//
//     SUBi  Rx, 1
//     CMPi  Rx, 0    ;  redundant — SUBi already set Z/N
//     BNE   loop
//
// are common.  The generic LLVM `PeepholeOptimizer` (added at -O1+ by
// `addMachineSSAOptimization`) drives compare-elimination via two virtual
// hooks on `TargetInstrInfo`: `analyzeCompare` (recognise the CMP) and
// `optimizeCompareInstr` (decide whether to delete it).
//
// The minimal pattern handled here:
//   1. `CMPi Rx, 0` whose only purpose is to set Z/N for a subsequent Bcc.
//   2. The most recent SR-touching predecessor in the same MBB writes Rx.
//   3. All SR readers between the CMP and the next SR redefinition use only
//      Z and N (i.e. BEQ/BNE/BMI/BPL).  C and V differ between
//      `SUBi Rx, k` and `CMP Rx, 0` whenever k != 0, so flag-readers that
//      consume C or V must keep the explicit CMP.
//
// Future extensions (left as TODOs):
//   - `CMP Rx, Ry` after `SUB Rx_dst, Ry`.
//   - `TESTi Rx, mask` after `ANDi Rx, mask`.
//   - Looking through trivial COPY chains so SUB-then-COPY-then-CMP elides.
//   - Cross-MBB analysis (currently scoped to one block).
//===----------------------------------------------------------------------===//

namespace {

// Bitmask of SR flags read by each Bcc opcode.  Bit 0 = Z, 1 = N, 2 = C,
// 3 = V.  Branches outside this table conservatively report "all flags."
constexpr unsigned FZ = 0x1;
constexpr unsigned FN = 0x2;
constexpr unsigned FC = 0x4;
constexpr unsigned FV = 0x8;

unsigned getBranchFlagsRead(unsigned Opcode) {
  switch (Opcode) {
  case Penumbra::BEQ:
  case Penumbra::BNE:
    return FZ;
  case Penumbra::BMI:
  case Penumbra::BPL:
    return FN;
  case Penumbra::BCS:
  case Penumbra::BCC:
    return FC;
  case Penumbra::BVS:
  case Penumbra::BVC:
    return FV;
  case Penumbra::BHI:
  case Penumbra::BLS:
    return FZ | FC;
  case Penumbra::BGE:
  case Penumbra::BLT:
    return FN | FV;
  case Penumbra::BGT:
  case Penumbra::BLE:
    return FZ | FN | FV;
  default:
    return FZ | FN | FC | FV;
  }
}

} // namespace

bool PenumbraInstrInfo::analyzeCompare(const MachineInstr &MI, Register &SrcReg,
                                       Register &SrcReg2, int64_t &CmpMask,
                                       int64_t &CmpValue) const {
  switch (MI.getOpcode()) {
  case Penumbra::CMPi:
    SrcReg = MI.getOperand(0).getReg();
    SrcReg2 = Register();
    CmpMask = ~0;
    CmpValue = MI.getOperand(1).getImm();
    return true;
  // TODO: CMP, TESTi, TEST when their elision patterns are added below.
  default:
    return false;
  }
}

bool PenumbraInstrInfo::optimizeCompareInstr(
    MachineInstr &CmpInstr, Register SrcReg, Register /*SrcReg2*/,
    int64_t /*CmpMask*/, int64_t CmpValue,
    const MachineRegisterInfo * /*MRI*/) const {
  // Currently we only know how to elide `CMPi Rx, 0`.
  if (CmpInstr.getOpcode() != Penumbra::CMPi || CmpValue != 0)
    return false;

  MachineBasicBlock *MBB = CmpInstr.getParent();

  // Walk back to the most recent SR-touching predecessor.  If it defines
  // SrcReg, its Z/N flags already encode `SrcReg vs 0`.  If it touches SR
  // but doesn't define SrcReg, SR has been clobbered and we must keep the
  // CMP.
  MachineInstr *Producer = nullptr;
  for (auto It = MachineBasicBlock::iterator(CmpInstr); It != MBB->begin();) {
    --It;
    if (It->isDebugInstr())
      continue;
    if (!It->modifiesRegister(Penumbra::SR, &getRegisterInfo()))
      continue;
    if (It->getNumOperands() > 0 && It->getOperand(0).isReg() &&
        It->getOperand(0).isDef() && It->getOperand(0).getReg() == SrcReg)
      Producer = &*It;
    break;
  }
  if (!Producer)
    return false;

  // Walk forward from the CMP and verify every SR reader consumes only Z
  // and N before the next SR clobber.
  for (auto It = std::next(MachineBasicBlock::iterator(CmpInstr));
       It != MBB->end(); ++It) {
    if (It->isDebugInstr())
      continue;
    if (It->readsRegister(Penumbra::SR, &getRegisterInfo())) {
      unsigned Flags = getBranchFlagsRead(It->getOpcode());
      if (Flags & ~(FZ | FN))
        return false;
    }
    if (It->modifiesRegister(Penumbra::SR, &getRegisterInfo()))
      break;
  }

  CmpInstr.eraseFromParent();
  return true;
}

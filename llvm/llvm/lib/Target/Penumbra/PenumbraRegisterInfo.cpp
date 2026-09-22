//===-- PenumbraRegisterInfo.cpp - Penumbra Register Info ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraRegisterInfo.h"
#include "PenumbraFrameLowering.h"
#include "PenumbraInstrInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

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

  const PenumbraFrameLowering *TFI = static_cast<const PenumbraFrameLowering *>(
      MF.getSubtarget().getFrameLowering());
  if (TFI->hasFP(MF))
    Reserved.set(Penumbra::R10); // Frame pointer

  return Reserved;
}

// Whether the flags hold a value some later instruction reads, just before MI.
static bool flagsLiveBefore(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MI,
                            const TargetRegisterInfo *TRI) {
  for (MachineBasicBlock::iterator I = MI, E = MBB.end(); I != E; ++I) {
    if (I->readsRegister(Penumbra::SR, TRI))
      return true;
    if (I->modifiesRegister(Penumbra::SR, TRI))
      return false;
  }
  return llvm::any_of(MBB.successors(), [](const MachineBasicBlock *Succ) {
    return Succ->isLiveIn(Penumbra::SR);
  });
}

// Returns the instruction that starts the flag value live at MI; an ADC or SBC
// continues the value it reads.  The errors guard invariants kept elsewhere:
// selection keeps each flag value within one block, and nothing in the body
// writes the frame base register.
static MachineBasicBlock::iterator
flagChainStart(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
               Register BaseReg, const TargetRegisterInfo *TRI) {
  for (MachineBasicBlock::iterator I = MI; I != MBB.begin();) {
    --I;
    if (I->modifiesRegister(BaseReg, TRI))
      report_fatal_error("frame base register written while flags are live");
    if (I->modifiesRegister(Penumbra::SR, TRI) &&
        !I->readsRegister(Penumbra::SR, TRI))
      return I;
  }
  report_fatal_error("flags live into block with a large stack offset");
}

bool PenumbraRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator MI,
                                               int SPAdj,
                                               unsigned FIOperandNum,
                                               RegScavenger *RS) const {
  MachineInstr &Inst = *MI;
  MachineBasicBlock &MBB = *Inst.getParent();
  MachineFunction &MF = *MBB.getParent();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const PenumbraFrameLowering *TFI = static_cast<const PenumbraFrameLowering *>(
      MF.getSubtarget().getFrameLowering());
  const PenumbraInstrInfo &TII =
      *static_cast<const PenumbraInstrInfo *>(MF.getSubtarget().getInstrInfo());

  // Calculate the actual offset.  The instruction's existing immediate
  // slot may already carry a non-zero constant (e.g. `STW r, [FI + 4]`
  // from folded struct/array accesses) — add it in rather than
  // overwriting, or the fold's constant is silently lost.
  int64_t Offset =
      MFI.getObjectOffset(Inst.getOperand(FIOperandNum).getIndex()) +
      Inst.getOperand(FIOperandNum + 1).getImm() +
      MFI.getStackSize() + SPAdj;

  // When FP is active, use R10 (FP) as the base register for normal frame
  // accesses.  But CSR spill/restore must use SP — they execute *before*
  // FP is set up (and *after* FP is torn down), so R10 doesn't yet (or
  // anymore) point at the frame.  PEI's assignCalleeSavedSpillSlots
  // allocates CSRs as a single contiguous batch of stack objects, so the
  // CSR frame indices form a monotonic range bounded by CSI.front() and
  // CSI.back() — a single range check identifies them (same trick as
  // RISCVFrameLowering::getFrameIndexReference).  Empty CSI yields the
  // dead range [0, -1] so the check correctly fails for every FI.
  int MinCSFI = 0;
  int MaxCSFI = -1;
  if (MFI.isCalleeSavedInfoValid()) {
    const auto &CSI = MFI.getCalleeSavedInfo();
    if (!CSI.empty()) {
      MinCSFI = CSI.front().getFrameIdx();
      MaxCSFI = CSI.back().getFrameIdx();
    }
  }
  int FI = Inst.getOperand(FIOperandNum).getIndex();
  bool IsCSRSlot = FI >= MinCSFI && FI <= MaxCSFI;
  Register BaseReg = (TFI->hasFP(MF) && !IsCSRSlot) ? Penumbra::R10
                                                    : Penumbra::R14;

  // Fast path: the 16-bit signed memory-offset field fits.
  if (isInt<16>(Offset)) {
    Inst.getOperand(FIOperandNum).ChangeToRegister(BaseReg, /*isDef=*/false);
    Inst.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
    return false;
  }

  // Large offset: materialize `BaseReg + Offset` into a scratch register
  // and rewrite the memory op to use `[scratch + 0]`.  This pass runs
  // after register allocation; creating a fresh virtual register here
  // is the pattern LLVM targets like RISC-V use — the register scavenger
  // (enabled by `requiresFrameIndexScavenging`) promotes it to a
  // physical register (or a scavenged spill slot) in a later pass.
  //
  // The ADD writes the flags, which may be live here: a reload between a
  // compare and its branch, or inside a 64-bit ADD/ADC chain.  Code cannot
  // save the flags around it (RDSPR is privileged and WRSPR SR reserved), so
  // where they are live the address is formed ahead of the instruction that
  // set them.
  MachineBasicBlock::iterator InsertPt = MI;
  if (flagsLiveBefore(MBB, MI, this))
    InsertPt = flagChainStart(MBB, MI, BaseReg, this);
  DebugLoc DL = Inst.getDebugLoc();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  Register Scratch = MRI.createVirtualRegister(&Penumbra::GPR_AllocatableRegClass);
  BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LLI), Scratch)
      .addImm(Offset & 0xFFFF);
  BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::LUI), Scratch)
      .addReg(Scratch)
      .addImm((Offset >> 16) & 0xFFFF);
  BuildMI(MBB, InsertPt, DL, TII.get(Penumbra::ADD), Scratch)
      .addReg(Scratch)
      .addReg(BaseReg);
  Inst.getOperand(FIOperandNum).ChangeToRegister(Scratch, /*isDef=*/false,
                                                 /*isImp=*/false,
                                                 /*isKill=*/true);
  Inst.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
  return false;
}

Register
PenumbraRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  const PenumbraFrameLowering *TFI = static_cast<const PenumbraFrameLowering *>(
      MF.getSubtarget().getFrameLowering());
  return TFI->hasFP(MF) ? Penumbra::R10 : Penumbra::R14;
}

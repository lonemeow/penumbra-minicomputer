//===-- PenumbraFrameLowering.cpp - Penumbra Frame Lowering ----------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFrameLowering.h"
#include "PenumbraInstrInfo.h"
#include "PenumbraRegisterInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

// R10 is used as frame pointer when the function has dynamic stack
// allocation (alloca/VLAs).  Without FP, SP-relative accesses break
// because alloca moves SP after the frame is set up.
static const MCPhysReg FPReg = Penumbra::R10;

bool PenumbraFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MFI.hasVarSizedObjects();
}

// Ensure R10 is saved/restored when used as FP.
void PenumbraFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                                  BitVector &SavedRegs,
                                                  RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  if (hasFP(MF))
    SavedRegs.set(FPReg);
}

// Emit `R14 = R14 <op> StackSize` for stack growth/shrink.  Frames up to
// 64 KiB use a single immediate-form instruction (SUBi/ADDi); larger frames
// materialize StackSize in scratch R11 and use the register form.  R11 is
// caller-save scratch and is dead both at function entry (no incoming live
// value) and just before function exit (no return value lives there), so
// it's always safe to clobber from prologue/epilogue.
static void adjustSP(MachineBasicBlock &MBB,
                     MachineBasicBlock::iterator MBBI, DebugLoc DL,
                     const PenumbraInstrInfo &TII, uint64_t StackSize,
                     unsigned ImmOpc, unsigned RegOpc) {
  if (isUInt<16>(StackSize)) {
    // <op>i r14, #StackSize
    BuildMI(MBB, MBBI, DL, TII.get(ImmOpc), Penumbra::R14)
        .addReg(Penumbra::R14)
        .addImm(StackSize);
    return;
  }
  // Large frame: three-instruction sequence.
  //   lli r11, lo16(StackSize)
  //   lui r11, hi16(StackSize)
  //   <op> r14, r11
  BuildMI(MBB, MBBI, DL, TII.get(Penumbra::LLI), Penumbra::R11)
      .addImm(StackSize & 0xFFFF);
  BuildMI(MBB, MBBI, DL, TII.get(Penumbra::LUI), Penumbra::R11)
      .addReg(Penumbra::R11)
      .addImm((StackSize >> 16) & 0xFFFF);
  BuildMI(MBB, MBBI, DL, TII.get(RegOpc), Penumbra::R14)
      .addReg(Penumbra::R14)
      .addReg(Penumbra::R11);
}

// Emit the function prologue: allocate the stack frame by subtracting
// StackSize from SP (R14).  When FP is needed, set FP = SP after
// frame allocation so that locals are accessible via [FP+offset]
// even after alloca moves SP.
void PenumbraFrameLowering::emitPrologue(MachineFunction &MF,
                                          MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (StackSize == 0 && !hasFP(MF))
    return;

  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;
  const auto &TII =
      *static_cast<const PenumbraInstrInfo *>(MF.getSubtarget().getInstrInfo());

  if (StackSize != 0)
    adjustSP(MBB, MBBI, DL, TII, StackSize,
             /*ImmOpc=*/Penumbra::SUBi, /*RegOpc=*/Penumbra::SUB);

  // FP setup (mov r10, r14) is emitted in spillCalleeSavedRegisters AFTER
  // all CSR spills have been placed.  Doing it here would clobber the
  // caller's R10 before PEI's STW R10, [...] runs, so the saved R10 would
  // be the new value (= SP), not the caller's value, and the function
  // would return with R10 pointing into its own (now-deallocated) frame.
}

// Emit the function epilogue: release the stack frame by adding StackSize
// back to SP before the return.  When FP is used, SP was already restored
// from FP by restoreCalleeSavedRegisters.
void PenumbraFrameLowering::emitEpilogue(MachineFunction &MF,
                                          MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (StackSize == 0)
    return;

  // Insert before the terminator (RET/JMP).
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  const auto &TII =
      *static_cast<const PenumbraInstrInfo *>(MF.getSubtarget().getInstrInfo());

  adjustSP(MBB, MBBI, DL, TII, StackSize,
           /*ImmOpc=*/Penumbra::ADDi, /*RegOpc=*/Penumbra::ADD);
}

// Spill CSRs at the prologue insertion point.  Mirrors AArch64's
// `getPrologueDeath` pattern: if a CSR is also marked live-in to the
// function, suppress the kill flag on its spill — the live-in status
// means a later instruction still needs the incoming value.  The main
// case is R13 when __builtin_return_address(0) seeds a capture MOV at
// the entry-block head; spilling R13 to the stack doesn't alter R13
// itself, so an over-eager kill annotation would falsely report the
// value as dead and trip the machine verifier.
bool PenumbraFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return true;

  MachineFunction &MF = *MBB.getParent();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const MachineRegisterInfo &MRI = MF.getRegInfo();

  for (const CalleeSavedInfo &CS : CSI) {
    Register Reg = CS.getReg();
    bool IsKill = !MRI.isLiveIn(Reg);
    TII.storeRegToStackSlot(MBB, MI, Reg, IsKill, CS.getFrameIdx(),
                             TRI->getMinimalPhysRegClass(Reg),
                             Register());
  }

  // Set up FP after all CSRs (including R10) are saved.  CSR spills
  // resolve to SP-relative addresses (see PenumbraRegisterInfo::
  // eliminateFrameIndex), so the spilled R10 captures the caller's
  // value before we overwrite it here.
  if (hasFP(MF)) {
    DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
    const auto &TII2 = *static_cast<const PenumbraInstrInfo *>(
        MF.getSubtarget().getInstrInfo());
    BuildMI(MBB, MI, DL, TII2.get(Penumbra::MOV), FPReg)
        .addReg(Penumbra::R14);
  }
  return true;
}

// Custom CSR restore for functions with FP.  We must:
//   1. Restore SP from FP (undo any alloca)
//   2. Restore all CSRs except FP using [FP+offset]
//   3. Restore FP last (its load still reads from valid FP)
// For non-FP functions, return false to use the default PEI handling.
bool PenumbraFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI,
    const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return true;

  MachineFunction &MF = *MBB.getParent();
  if (!hasFP(MF))
    return false; // default handling for non-FP functions

  const auto &TII =
      *static_cast<const PenumbraInstrInfo *>(MF.getSubtarget().getInstrInfo());
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();

  // 1. Restore SP from FP before any CSR restores.
  BuildMI(MBB, MI, DL, TII.get(Penumbra::MOV), Penumbra::R14)
      .addReg(FPReg);

  // 2. Restore all CSRs except FP.
  const CalleeSavedInfo *FPEntry = nullptr;
  for (const CalleeSavedInfo &CS : CSI) {
    if (CS.getReg() == FPReg) {
      FPEntry = &CS;
      continue;
    }
    TII.loadRegFromStackSlot(MBB, MI, CS.getReg(), CS.getFrameIdx(),
                             TRI->getMinimalPhysRegClass(CS.getReg()),
                             Register());
  }

  // 3. Restore FP last.  The LDW reads from [FP+offset] while FP is
  //    still the frame pointer; the loaded value then overwrites FP
  //    with the caller's saved value.
  if (FPEntry)
    TII.loadRegFromStackSlot(MBB, MI, FPEntry->getReg(),
                             FPEntry->getFrameIdx(),
                             TRI->getMinimalPhysRegClass(FPEntry->getReg()),
                             Register());

  return true;
}

// ADJCALLSTACKDOWN/UP are absorbed into the prologue/epilogue stack
// allocation — just erase them here.
MachineBasicBlock::iterator
PenumbraFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator MI) const {
  return MBB.erase(MI);
}

// Pre-allocate an emergency spill slot for the RegScavenger — but only
// when the frame is big enough that eliminateFrameIndex might need one.
// Small frames (16-bit offset fits in the memory-offset field) never
// take the scratch path, so adding a slot there is pure bloat
// (every leaf function would pay +4 bytes of SP adjust).
// Follows the RISC-V pattern of using estimateStackSize with a
// slightly pessimistic threshold, since the final StackSize is only
// known after this hook runs and can grow a bit due to CSR-save
// padding and alignment.
void PenumbraFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *RS) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  if (isInt<15>(MFI.estimateStackSize(MF)))
    return; // comfortably within 16-bit signed offset; no scratch needed

  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const TargetRegisterClass *RC = &Penumbra::GPR_AllocatableRegClass;
  int FI = MFI.CreateStackObject(TRI->getSpillSize(*RC),
                                 TRI->getSpillAlign(*RC),
                                 /*isSpillSlot=*/true);
  RS->addScavengingFrameIndex(FI);
}

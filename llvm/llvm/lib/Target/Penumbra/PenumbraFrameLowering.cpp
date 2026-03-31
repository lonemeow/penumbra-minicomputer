//===-- PenumbraFrameLowering.cpp - Penumbra Frame Lowering ----------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFrameLowering.h"
#include "PenumbraInstrInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

// Emit the function prologue: allocate the stack frame by subtracting
// StackSize from SP (R14).  Callee-saved register spills are inserted
// separately by PrologEpilogInserter, which calls storeRegToStackSlot.
void PenumbraFrameLowering::emitPrologue(MachineFunction &MF,
                                          MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t StackSize = MFI.getStackSize();
  if (StackSize == 0)
    return;

  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;
  const auto &TII =
      *static_cast<const PenumbraInstrInfo *>(MF.getSubtarget().getInstrInfo());

  // sub r14, #StackSize  — grow stack downward
  BuildMI(MBB, MBBI, DL, TII.get(Penumbra::SUBi), Penumbra::R14)
      .addReg(Penumbra::R14)
      .addImm(StackSize);
}

// Emit the function epilogue: release the stack frame by adding StackSize
// back to SP before the return.  Callee-saved register reloads are inserted
// by PrologEpilogInserter before this runs.
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

  // add r14, #StackSize  — shrink stack back
  BuildMI(MBB, MBBI, DL, TII.get(Penumbra::ADDi), Penumbra::R14)
      .addReg(Penumbra::R14)
      .addImm(StackSize);
}

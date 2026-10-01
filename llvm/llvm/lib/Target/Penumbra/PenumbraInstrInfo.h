//===-- PenumbraInstrInfo.h - Penumbra Instruction Info ----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAINSTRINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAINSTRINFO_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "PenumbraGenInstrInfo.inc"

namespace llvm {

class TargetRegisterInfo;
class TargetSubtargetInfo;

class PenumbraInstrInfo : public PenumbraGenInstrInfo {
public:
  PenumbraInstrInfo(const TargetSubtargetInfo &STI,
                    const TargetRegisterInfo &TRI);

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  void storeRegToStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
      bool IsKill, int FrameIndex, const TargetRegisterClass *RC,
      Register VReg,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  bool expandPostRAPseudo(MachineInstr &MI) const override;

  // Whether a wide constant counts as cheap is the tuned core's answer
  // (PenumbraSubtarget::preferRecomputingWideConstants).
  bool isAsCheapAsAMove(const MachineInstr &MI) const override;

  void loadRegFromStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
      int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      unsigned SubReg = 0,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify = false) const override;

  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;

  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;

  bool
  reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const override;

  // Compare-elimination peephole hooks (driven by PeepholeOptimizer).  Together
  // they recognize `CMPi Rx, 0` whose Z/N flags are already produced by an
  // immediately-preceding flag-setting ALU instruction, and a wrap check
  // comparing an add's sum against an addend, and erase the CMP.
  bool analyzeCompare(const MachineInstr &MI, Register &SrcReg,
                      Register &SrcReg2, int64_t &CmpMask,
                      int64_t &CmpValue) const override;

  bool optimizeCompareInstr(MachineInstr &CmpInstr, Register SrcReg,
                            Register SrcReg2, int64_t CmpMask,
                            int64_t CmpValue,
                            const MachineRegisterInfo *MRI) const override;

private:
  static unsigned getOppositeBranchOpcode(unsigned Opc);

  // Erase a compare of an add's sum against one of its addends, taking the
  // add's carry instead.
  bool optimizeWrapCheck(MachineInstr &CmpInstr, Register SumReg,
                         const MachineRegisterInfo &MRI) const;
};

namespace Penumbra {

// How the flags, as they stand before some instruction, are used from there to
// the end of its block.  Flags are consumed in the block that sets them -- the
// select expansion keeps its compare and branch together -- so code that
// splits a block between a flag producer and its reader must add SR to the new
// block's live-ins.
struct FlagUses {
  // The instructions that read them, in block order.
  SmallVector<MachineInstr *, 2> Readers;
  // The instruction that overwrites them, or the block's end.  An instruction
  // that reads the flags and writes new ones, as ADC does, is the last reader
  // too.
  MachineBasicBlock::iterator Overwriter;
  // They reach the block's end and a successor names SR as a live-in.
  bool LiveOut = false;

  bool live() const { return !Readers.empty() || LiveOut; }
};

FlagUses flagUsesFrom(MachineBasicBlock &MBB, MachineBasicBlock::iterator I);

// Whether anything reads the flags as they stand before I.
bool flagsLiveBefore(const MachineBasicBlock &MBB,
                     MachineBasicBlock::const_iterator I);

} // namespace Penumbra

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAINSTRINFO_H

//===-- PenumbraISelLowering.h - Penumbra TargetLowering hooks ----*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/IR/InlineAsm.h"

namespace llvm {

class PenumbraSubtarget;

class PenumbraISelLowering : public TargetLowering {
public:
  PenumbraISelLowering(const TargetMachine &TM, const PenumbraSubtarget &STI);

  CCAssignFn *getCCAssignFn(CallingConv::ID CC, bool Return,
                             bool IsVarArg) const;

  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                              MachineBasicBlock *MBB) const override;

  // Marks the flag definitions nothing reads as dead, once selection and the
  // select expansion have each built their instructions.
  void finalizeLowering(MachineFunction &MF) const override;

  // Hold G_SCMP/G_UCMP on LegalizerHelper's select-chain expansion.  Declaring
  // ZeroOrOneBooleanContent otherwise switches it to `zext(gt) - zext(lt)`,
  // which measures worse here: the select chain folds each comparison into a
  // SELECT_CC diamond instead of materialising both booleans into GPRs.
  bool preferSelectsOverBooleanArithmetic(EVT VT) const override;

  // Known bits of the target generic opcodes: G_ICMP_PAIR is a 0/1 boolean.
  void computeKnownBitsForTargetInstr(GISelValueTracking &Analysis, Register R,
                                      KnownBits &Known,
                                      const APInt &DemandedElts,
                                      const MachineRegisterInfo &MRI,
                                      unsigned Depth = 0) const override;

  // Jump table encoding: always use label differences.
  // This avoids relocations, works at any load address (PIC/PIE/static),
  // and enables future optimization to 16-bit entries.
  unsigned getJumpTableEncoding() const override;

  // Exception handling: personality function returns exception pointer
  // in R1, selector in R2 (matching the calling convention).
  Register
  getExceptionPointerRegister(const Constant *PersonalityFn) const override;
  Register
  getExceptionSelectorRegister(const Constant *PersonalityFn) const override;

  // Inline assembly support.
  ConstraintType getConstraintType(StringRef Constraint) const override;
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                               StringRef Constraint, MVT VT) const override;

  // Named global register variables (`register T x __asm("r12")`).  The
  // kernel uses this to pin curlwp in the thread-pointer register R12.
  Register getRegisterByName(const char *RegName, LLT Ty,
                             const MachineFunction &MF) const override;

  bool isIntDivCheap(EVT VT, AttributeList Attr) const override;

  // Whether CodeGenPrepare rewrites an add and its wrap check into an overflow
  // intrinsic.  At register width it must not: selection has no branch on an
  // overflow op's carry, so the carry would be materialised and tested, while
  // the compare the check started as selects next to its branch, where
  // compare elimination can reuse the add's flags.  A wider add narrows onto
  // the ADD/ADC carry chain, whose final carry is the wrap, and that beats the
  // multi-word compare the check would otherwise expand to.
  bool shouldFormOverflowOp(unsigned Opcode, EVT VT,
                            bool MathUsed) const override {
    if (VT.isScalarInteger() && VT.getSizeInBits() <= 32)
      return false;
    return TargetLowering::shouldFormOverflowOp(Opcode, VT, MathUsed);
  }

  // Which entry-block instructions the Localizer re-emits in the blocks that
  // use them.
  bool shouldLocalize(const MachineInstr &MI,
                      const TargetTransformInfo *TTI) const override;

  // LSR / CodeGenPrepare addressing-mode cost: report what addressing
  // shapes the load/store M-format encoding can express in a single
  // instruction.  The default in TargetLoweringBase claims a "RISCy r+r
  // and r+i" model — we only have r+i (HasBaseReg + signed 16-bit
  // displacement), so reporting the default would let LSR rewrite
  // pointer-bump loops into base+index form, costing two extra ADDs
  // per iteration.
  bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM,
                             Type *Ty, unsigned AS,
                             Instruction *I) const override;

  // The access type an inline memcpy, memmove or memset is built from.
  LLT getOptimalMemOpLLT(const MemOp &Op,
                         const AttributeList &FuncAttributes) const override;

private:
  const PenumbraSubtarget &Subtarget;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

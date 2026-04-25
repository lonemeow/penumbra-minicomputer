//===-- PenumbraISelLowering.h - Penumbra DAG Lowering Interface --*- C++ -*-===//
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

  bool isIntDivCheap(EVT VT, AttributeList Attr) const override;

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
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

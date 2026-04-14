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
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRAISELLOWERING_H

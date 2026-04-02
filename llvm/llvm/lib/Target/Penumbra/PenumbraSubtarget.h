//===-- PenumbraSubtarget.h - Penumbra Subtarget Info ----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H

#include "GISel/PenumbraCallLowering.h"
#include "GISel/PenumbraLegalizerInfo.h"
#include "GISel/PenumbraRegisterBankInfo.h"
#include "PenumbraFrameLowering.h"
#include "PenumbraISelLowering.h"
#include "PenumbraInstrInfo.h"
#include "PenumbraRegisterInfo.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/InlineAsmLowering.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"

#define GET_SUBTARGETINFO_HEADER
#include "PenumbraGenSubtargetInfo.inc"

namespace llvm {

class PenumbraSubtarget : public PenumbraGenSubtargetInfo {
  // RegInfo must be declared before InstrInfo — InstrInfo constructor needs TRI.
  PenumbraRegisterInfo RegInfo;
  PenumbraInstrInfo InstrInfo;
  PenumbraFrameLowering FrameLowering;
  PenumbraISelLowering TLInfo;
  PenumbraLegalizerInfo Legalizer;
  PenumbraRegisterBankInfo RegBankInfo;
  std::unique_ptr<CallLowering> CallLoweringInfo;
  std::unique_ptr<InlineAsmLowering> InlineAsmLoweringInfo;
  std::unique_ptr<InstructionSelector> InstSelector;

public:
  PenumbraSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                    const TargetMachine &TM);

  const PenumbraInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const PenumbraFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const PenumbraRegisterInfo *getRegisterInfo() const override {
    return &RegInfo;
  }
  const PenumbraISelLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const RegisterBankInfo *getRegBankInfo() const override {
    return &RegBankInfo;
  }
  const LegalizerInfo *getLegalizerInfo() const override {
    return &Legalizer;
  }
  const CallLowering *getCallLowering() const override {
    return CallLoweringInfo.get();
  }
  const InlineAsmLowering *getInlineAsmLowering() const override {
    return InlineAsmLoweringInfo.get();
  }
  InstructionSelector *getInstructionSelector() const override {
    return InstSelector.get();
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H

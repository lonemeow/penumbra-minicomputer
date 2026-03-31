//===-- PenumbraSubtarget.h - Penumbra Subtarget Info ----------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H
#define LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H

#include "PenumbraFrameLowering.h"
#include "PenumbraISelLowering.h"
#include "PenumbraInstrInfo.h"
#include "PenumbraRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"

#define GET_SUBTARGETINFO_HEADER
#include "PenumbraGenSubtargetInfo.inc"

namespace llvm {

class PenumbraSubtarget : public PenumbraGenSubtargetInfo {
  PenumbraInstrInfo InstrInfo;
  PenumbraFrameLowering FrameLowering;
  PenumbraRegisterInfo RegInfo;
  PenumbraISelLowering TLInfo;

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
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_PENUMBRASUBTARGET_H

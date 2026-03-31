//===-- PenumbraSubtarget.cpp - Penumbra Subtarget Info --------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraSubtarget.h"

#define GET_SUBTARGETINFO_CTOR
#include "PenumbraGenSubtargetInfo.inc"

using namespace llvm;

PenumbraSubtarget::PenumbraSubtarget(const Triple &TT, StringRef CPU,
                                      StringRef FS, const TargetMachine &TM)
    : PenumbraGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS),
      RegInfo(),
      InstrInfo(*this, RegInfo),
      FrameLowering(),
      TLInfo(TM, *this),
      Legalizer(*this),
      RegBankInfo(*getRegisterInfo()) {}

//===-- PenumbraSubtarget.cpp - Penumbra Subtarget Info --------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraSubtarget.h"
#include "GISel/PenumbraCallLowering.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"

namespace llvm {
// Defined in GISel/PenumbraInstructionSelector.cpp.
InstructionSelector *
createPenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                                  const PenumbraSubtarget &STI,
                                  const PenumbraRegisterBankInfo &RBI);
} // namespace llvm

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
      RegBankInfo(*getRegisterInfo()) {
  CallLoweringInfo.reset(new PenumbraCallLowering(*getTargetLowering()));
  InstSelector.reset(createPenumbraInstructionSelector(
      static_cast<const PenumbraTargetMachine &>(TM), *this, RegBankInfo));
}

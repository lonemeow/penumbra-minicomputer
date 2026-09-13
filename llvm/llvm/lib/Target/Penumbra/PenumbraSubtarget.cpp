//===-- PenumbraSubtarget.cpp - Penumbra Subtarget Info --------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraSubtarget.h"
#include "GISel/PenumbraCallLowering.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"

namespace llvm {
// Defined in GISel/PenumbraInstructionSelector.cpp.
InstructionSelector *
createPenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                                  const PenumbraSubtarget &STI,
                                  const PenumbraRegisterBankInfo &RBI);
} // namespace llvm

// TARGET_DESC carries the ParseSubtargetFeatures definition, CTOR the
// generated base constructor.
#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "PenumbraGenSubtargetInfo.inc"

using namespace llvm;

PenumbraSubtarget &
PenumbraSubtarget::initializeSubtargetDependencies(StringRef CPU,
                                                   StringRef TuneCPU,
                                                   StringRef FS) {
  ParseSubtargetFeatures(resolveCPU(CPU), resolveTuneCPU(CPU, TuneCPU), FS);
  return *this;
}

PenumbraSubtarget::PenumbraSubtarget(const Triple &TT, StringRef CPU,
                                      StringRef TuneCPU, StringRef FS,
                                      const TargetMachine &TM)
    : PenumbraGenSubtargetInfo(TT, resolveCPU(CPU),
                               resolveTuneCPU(CPU, TuneCPU), FS),
      RegInfo(),
      // The feature fields have to be set before anything that can read one is
      // built.  Threading the call through the first member that takes an
      // argument is what orders it ahead of the rest.
      InstrInfo(initializeSubtargetDependencies(CPU, TuneCPU, FS), RegInfo),
      FrameLowering(),
      TLInfo(TM, *this),
      Legalizer(*this),
      RegBankInfo(*getRegisterInfo()) {
  CallLoweringInfo.reset(new PenumbraCallLowering(*getTargetLowering()));
  InlineAsmLoweringInfo.reset(new InlineAsmLowering(getTargetLowering()));
  InstSelector.reset(createPenumbraInstructionSelector(
      static_cast<const PenumbraTargetMachine &>(TM), *this, RegBankInfo));
}

// Every Penumbra scheduling model states MUL and DIV latencies, and the models
// are otherwise incomplete, so a missing entry reads back as zero cycles —
// a free multiply.  Fail instead of pricing it that way.
static unsigned getModelCycles(const PenumbraSubtarget &STI, unsigned Opcode) {
  unsigned SchedClass = STI.getInstrInfo()->get(Opcode).getSchedClass();
  int Cycles = STI.getSchedModel().computeInstrLatency(STI, SchedClass);
  if (Cycles <= 0)
    reportFatalInternalError("Penumbra scheduling model states no latency for "
                             "a divmul instruction");
  return Cycles;
}

unsigned PenumbraSubtarget::getMulCycles() const {
  return getModelCycles(*this, Penumbra::MUL);
}

unsigned PenumbraSubtarget::getDivCycles() const {
  return getModelCycles(*this, Penumbra::DIV);
}

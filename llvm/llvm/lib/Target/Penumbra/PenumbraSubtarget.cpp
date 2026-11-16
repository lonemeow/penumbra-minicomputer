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
#include <cmath>

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

static const MCSchedClassDesc *getSchedClassDesc(const PenumbraSubtarget &STI,
                                                 unsigned Opcode) {
  return STI.getSchedModel().getSchedClassDesc(
      STI.getInstrInfo()->get(Opcode).getSchedClass());
}

// The cycles an instruction of the opcode's class holds the core: its
// reciprocal throughput.
static unsigned getIssueCycles(const PenumbraSubtarget &STI, unsigned Opcode) {
  const MCSchedClassDesc *SC = getSchedClassDesc(STI, Opcode);
  // Without resource figures, getReciprocalThroughput falls back to a
  // micro-op count rather than failing.
  if (!SC->isValid() || SC->NumWriteProcResEntries == 0)
    reportFatalInternalError("Penumbra scheduling model states no issue "
                             "figure for a costed instruction class");
  return std::lround(MCSchedModel::getReciprocalThroughput(STI, *SC));
}

unsigned PenumbraSubtarget::getALUCycles() const {
  return getIssueCycles(*this, Penumbra::ADD);
}

unsigned PenumbraSubtarget::getMulCycles() const {
  return getIssueCycles(*this, Penumbra::MUL);
}

unsigned PenumbraSubtarget::getDivCycles() const {
  return getIssueCycles(*this, Penumbra::DIV);
}

unsigned PenumbraSubtarget::getWideConstantCycles() const {
  const MCWriteLatencyEntry *Result =
      getWriteLatencyEntry(getSchedClassDesc(*this, Penumbra::LLI), 0);
  // LUI's first register read is its own destination: the LLI result.  It
  // waits for whatever part of that latency LLI's own issue does not cover.
  int Advance = getReadAdvanceCycles(getSchedClassDesc(*this, Penumbra::LUI),
                                     /*UseIdx=*/0, Result->WriteResourceID);
  int LLIIssue = getIssueCycles(*this, Penumbra::LLI);
  int Wait = std::max(int(Result->Cycles) - Advance - LLIIssue, 0);
  return LLIIssue + Wait + getIssueCycles(*this, Penumbra::LUI);
}

bool PenumbraSubtarget::preferRecomputingWideConstants() const {
  return getWideConstantCycles() <= 2 * getALUCycles();
}

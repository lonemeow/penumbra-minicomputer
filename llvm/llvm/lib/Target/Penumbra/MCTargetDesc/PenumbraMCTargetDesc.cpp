//===-- PenumbraMCTargetDesc.cpp - Penumbra Target Descriptions -----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCTargetDesc.h"
#include "PenumbraMCAsmInfo.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#include "PenumbraGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "PenumbraGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "PenumbraGenRegisterInfo.inc"

static MCInstrInfo *createPenumbraMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitPenumbraMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createPenumbraMCRegisterInfo(const Triple & /*TT*/) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitPenumbraMCRegisterInfo(X, Penumbra::R13); // RA = R13 (link register)
  return X;
}

static MCSubtargetInfo *
createPenumbraMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  if (CPU.empty())
    CPU = "penumbra1";
  return createPenumbraMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCAsmInfo *createPenumbraMCAsmInfo(const MCRegisterInfo & /*MRI*/,
                                          const Triple &TT,
                                          const MCTargetOptions & /*Opts*/) {
  return new PenumbraMCAsmInfo(TT);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTargetMC() {
  Target &T = getThePenumbraTarget();

  RegisterMCAsmInfoFn X(T, createPenumbraMCAsmInfo);

  TargetRegistry::RegisterMCInstrInfo(T, createPenumbraMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createPenumbraMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createPenumbraMCSubtargetInfo);
}

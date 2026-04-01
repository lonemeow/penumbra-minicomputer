//===-- PenumbraTargetMachine.cpp - Penumbra Target Machine ---------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraTargetMachine.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

static const char *PenumbraDataLayout =
    "e"        // little-endian
    "-m:e"     // ELF mangling
    "-p:32:32" // 32-bit pointers, 32-bit aligned
    "-i32:32"  // i32 is 32-bit aligned
    "-i64:64"  // i64 is 64-bit aligned (for long long)
    "-n32"     // native integer width is 32
    "-S32";    // stack is 32-bit aligned

// ── PassConfig ───────────────────────────────────────────────────────────────

namespace {
class PenumbraPassConfig : public TargetPassConfig {
public:
  PenumbraPassConfig(PenumbraTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  // GlobalISel pipeline — four mandatory passes in order.
  bool addIRTranslator() override {
    addPass(new IRTranslator());
    return false;
  }
  bool addLegalizeMachineIR() override {
    addPass(new Legalizer());
    return false;
  }
  bool addRegBankSelect() override {
    addPass(new RegBankSelect());
    return false;
  }
  bool addGlobalInstructionSelect() override {
    addPass(new InstructionSelect());
    return false;
  }
};
} // namespace

TargetPassConfig *
PenumbraTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new PenumbraPassConfig(*this, PM);
}

// ── TargetMachine ─────────────────────────────────────────────────────────────

PenumbraTargetMachine::PenumbraTargetMachine(
    const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
    const TargetOptions &Options, std::optional<Reloc::Model> RM,
    std::optional<CodeModel::Model> CM, CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, PenumbraDataLayout, TT,
                               CPU.empty() ? "penumbra1" : CPU, FS, Options,
                               RM.value_or(Reloc::Static),
                               CM.value_or(CodeModel::Small), OL),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, CPU.empty() ? "penumbra1" : CPU, FS, *this) {
  initAsmInfo();
  setGlobalISel(true);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTarget() {
  RegisterTargetMachine<PenumbraTargetMachine> X(getThePenumbraTarget());
  PassRegistry *PR = PassRegistry::getPassRegistry();
  initializeGlobalISel(*PR);
}

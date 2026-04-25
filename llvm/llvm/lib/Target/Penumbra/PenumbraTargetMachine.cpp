//===-- PenumbraTargetMachine.cpp - Penumbra Target Machine ---------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraTargetMachine.h"
#include "PenumbraTargetTransformInfo.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

// Forward declarations for combiner pass factories.  Matches the existing
// convention in PenumbraSubtarget.cpp for createPenumbraInstructionSelector —
// no Penumbra.h is provided, so each .cpp file that needs these declares
// them locally.
namespace llvm {
FunctionPass *createPenumbraPreLegalizerCombiner();
FunctionPass *createPenumbraO0PreLegalizerCombiner();
FunctionPass *createPenumbraPostLegalizerCombiner();
void initializePenumbraPreLegalizerCombinerPass(PassRegistry &);
void initializePenumbraO0PreLegalizerCombinerPass(PassRegistry &);
void initializePenumbraPostLegalizerCombinerPass(PassRegistry &);
} // namespace llvm

// ── Target Object File ──────────────────────────────────────────────────────
// Override one method: place jump tables in the function section so that
// label-difference entries (.word target - JT_base) can be resolved by
// the assembler without cross-section relocations.

namespace {
class PenumbraTargetObjectFile : public TargetLoweringObjectFileELF {
public:
  bool shouldPutJumpTableInFunctionSection(bool UsesLabelDifference,
                                           const Function &F) const override {
    // Always inline JT in .text so label-difference entries can be
    // resolved by the assembler within one section.  We emit label
    // differences from emitJumpTableEntry regardless of the MIR
    // encoding, so the JT must always be in the function section.
    return true;
  }
};
} // namespace

static const char *PenumbraDataLayout =
    "e"        // little-endian
    "-m:e"     // ELF mangling
    "-p:32:32" // 32-bit pointers, 32-bit aligned
    "-i32:32"  // i32 is 32-bit aligned
    "-i64:64"  // i64 is 64-bit aligned (for long long)
    "-n32"     // native integer width is 32
    "-S32";    // stack is 32-bit aligned

// ── TLS Lowering Pass ────────────────────────────────────────────────────────
// Lower @llvm.threadlocal.address intrinsics before GlobalISel runs.
//
// General-Dynamic model (PIC / shared libraries):
//   Replace with a call to __tls_get_addr.  The instruction selector
//   materialises the GOT tls_index address; the dynamic linker resolves it.
//
// Local-Exec / Initial-Exec model (static binaries):
//   Leave the intrinsic in place — the IRTranslator lowers it to
//   G_GLOBAL_VALUE, and the instruction selector emits
//   LLI+LUI (TP offset) + ADD R12 (TP register).

namespace {
class PenumbraLowerTLS : public FunctionPass {
  const TargetMachine *TM = nullptr;

public:
  static char ID;
  PenumbraLowerTLS() : FunctionPass(ID) {}
  explicit PenumbraLowerTLS(const TargetMachine *TM)
      : FunctionPass(ID), TM(TM) {}

  bool runOnFunction(Function &F) override {
    SmallVector<IntrinsicInst *, 16> GDIntrinsics;
    SmallVector<IntrinsicInst *, 16> LEIntrinsics;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
          if (II->getIntrinsicID() == Intrinsic::threadlocal_address) {
            auto *GV = dyn_cast<GlobalValue>(
                II->getArgOperand(0)->stripPointerCasts());
            TLSModel::Model Model = TM
                ? TM->getTLSModel(GV)
                : TLSModel::GeneralDynamic;
            // PIC code uses GD (__tls_get_addr with GOT-PCREL pattern).
            // The 5-instruction GOT-PCREL sequence can be relaxed by
            // lld to inline LE (MOV TP + TPREL) for static linking.
            // Non-PIC code uses LE directly (PtrToInt resolves to
            // absolute TLS relocs, linker resolves as R_TPREL).
            bool IsPIC = TM && TM->getRelocationModel() == Reloc::PIC_;
            if (IsPIC && Model != TLSModel::LocalExec) {
              GDIntrinsics.push_back(II);
            } else {
              LEIntrinsics.push_back(II);
            }
          }
        }
      }
    }

    if (GDIntrinsics.empty() && LEIntrinsics.empty())
      return false;

    Module *M = F.getParent();
    Type *PtrType = PointerType::getUnqual(F.getContext());
    Type *I32Type = Type::getInt32Ty(F.getContext());

    // GD/LD: replace with call to __tls_get_addr.
    if (!GDIntrinsics.empty()) {
      FunctionType *PtrToPtrFuncType =
          FunctionType::get(PtrType, {PtrType}, false);
      FunctionCallee TlsGetAddrFunc =
          M->getOrInsertFunction("__tls_get_addr", PtrToPtrFuncType);
      for (auto *II : GDIntrinsics) {
        IRBuilder<> Builder(II);
        Value *TLSAddr = II->getArgOperand(0);
        CallInst *NewCall = Builder.CreateCall(TlsGetAddrFunc, {TLSAddr});
        II->replaceAllUsesWith(NewCall);
        II->eraseFromParent();
      }
    }

    // LE/IE: emit TP + offset inline (no function call).
    // Read R12 (TP register) via inline asm, add the TP-relative
    // offset (linker resolves TLS GD relocs as R_TPREL for static).
    if (!LEIntrinsics.empty()) {
      FunctionType *ReadTPTy = FunctionType::get(I32Type, false);
      InlineAsm *ReadTP = InlineAsm::get(ReadTPTy,
          "mov $0, r12", "=r", /*hasSideEffects=*/false);
      for (auto *II : LEIntrinsics) {
        IRBuilder<> Builder(II);
        Value *TLSVar = II->getArgOperand(0);
        Value *TPVal = Builder.CreateCall(ReadTP);
        Value *Offset = Builder.CreatePtrToInt(TLSVar, I32Type);
        Value *Addr = Builder.CreateIntToPtr(
            Builder.CreateAdd(TPVal, Offset), PtrType);
        II->replaceAllUsesWith(Addr);
        II->eraseFromParent();
      }
    }

    return true;
  }
};
char PenumbraLowerTLS::ID = 0;
} // namespace

// ── PassConfig ───────────────────────────────────────────────────────────────

namespace {
class PenumbraPassConfig : public TargetPassConfig {
public:
  PenumbraPassConfig(PenumbraTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  // Pre-GlobalISel IR passes.
  void addIRPasses() override {
    addPass(createAtomicExpandLegacyPass());
    addPass(new PenumbraLowerTLS(&getTM<PenumbraTargetMachine>()));
    TargetPassConfig::addIRPasses();
  }

  // GlobalISel pipeline.  Mandatory passes (IRTranslator, Legalizer,
  // RegBankSelect, InstructionSelect) bracket optional combiner passes
  // that run before legalization and before reg-bank selection.
  bool addIRTranslator() override {
    addPass(new IRTranslator());
    return false;
  }
  void addPreLegalizeMachineIR() override {
    if (getOptLevel() == CodeGenOptLevel::None)
      addPass(createPenumbraO0PreLegalizerCombiner());
    else
      addPass(createPenumbraPreLegalizerCombiner());
  }
  bool addLegalizeMachineIR() override {
    addPass(new Legalizer());
    return false;
  }
  void addPreRegBankSelect() override {
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(createPenumbraPostLegalizerCombiner());
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

// Without this override, TargetMachine::getTargetTransformInfo returns a
// generic TTI(DataLayout) that doesn't know our TargetLowering exists, so
// LSR / CodeGenPrepare answer cost-model queries from the upstream
// "RISCy r+r and r+i" defaults — pessimising pointer-bump loops into
// base+index form.  Wiring PenumbraTTIImpl in routes those queries
// through PenumbraTargetLowering::isLegalAddressingMode.
TargetTransformInfo
PenumbraTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<PenumbraTTIImpl>(this, F));
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
      TLOF(std::make_unique<PenumbraTargetObjectFile>()),
      Subtarget(TT, CPU.empty() ? "penumbra1" : CPU, FS, *this) {
  initAsmInfo();
  setGlobalISel(true);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTarget() {
  RegisterTargetMachine<PenumbraTargetMachine> X(getThePenumbraTarget());
  PassRegistry *PR = PassRegistry::getPassRegistry();
  initializeGlobalISel(*PR);
  initializePenumbraPreLegalizerCombinerPass(*PR);
  initializePenumbraO0PreLegalizerCombinerPass(*PR);
  initializePenumbraPostLegalizerCombinerPass(*PR);
}

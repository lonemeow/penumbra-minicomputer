//===-- PenumbraMCTargetDesc.cpp - Penumbra Target Descriptions -----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCTargetDesc.h"
#include "PenumbraInstPrinter.h"
#include "PenumbraMCAsmInfo.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define GET_INSTRINFO_MC_HELPER_DEFS
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

//===----------------------------------------------------------------------===//
// MCInstrAnalysis — branch targets + lli/lui address resolution
//===----------------------------------------------------------------------===//

namespace {
class PenumbraMCInstrAnalysis : public MCInstrAnalysis {
  // GPR state tracking for lli/lui pair resolution.
  // Index 0 = R0 (hardwired zero, never tracked).
  mutable int64_t GPRState[16] = {};
  mutable bool GPRValid[16] = {};

  // Map MCRegister to hardware index (1–15), or nullopt for R0/non-GPR.
  static std::optional<unsigned> regIndex(unsigned Reg) {
    if (Reg < Penumbra::R0 || Reg > Penumbra::R15)
      return std::nullopt;
    unsigned Idx = Reg - Penumbra::R0;
    if (Idx == 0)
      return std::nullopt; // R0 is zero register
    return Idx;
  }

public:
  explicit PenumbraMCInstrAnalysis(const MCInstrInfo *Info)
      : MCInstrAnalysis(Info) {}

  void resetState() override {
    std::fill(std::begin(GPRValid), std::end(GPRValid), false);
  }

  void updateState(const MCInst &Inst, uint64_t Addr) override {
    // Terminators and calls clobber registers — clear all tracking.
    if (isTerminator(Inst) || isCall(Inst)) {
      resetState();
      return;
    }

    unsigned Opc = Inst.getOpcode();

    // LLI Rd, imm16 — Rd = zero_extend(imm16).
    // Ops: [0]=Rd, [1]=imm16.
    if (Opc == Penumbra::LLI) {
      if (auto Idx = regIndex(Inst.getOperand(0).getReg())) {
        GPRState[*Idx] = Inst.getOperand(1).getImm();
        GPRValid[*Idx] = true;
      }
      return;
    }

    // LLIS Rd, simm16 — Rd = sign_extend(imm16).
    // Ops: [0]=Rd, [1]=simm16.
    if (Opc == Penumbra::LLIS) {
      if (auto Idx = regIndex(Inst.getOperand(0).getReg())) {
        GPRState[*Idx] = Inst.getOperand(1).getImm();
        GPRValid[*Idx] = true;
      }
      return;
    }

    // LUI Rd, imm16 — Rd = (Rd & 0xFFFF) | (imm16 << 16).
    // Ops: [0]=Rd, [1]=Rd_in (tied), [2]=imm16.
    if (Opc == Penumbra::LUI) {
      if (auto Idx = regIndex(Inst.getOperand(0).getReg())) {
        if (GPRValid[*Idx])
          GPRState[*Idx] = (GPRState[*Idx] & 0xFFFF) |
                           (Inst.getOperand(2).getImm() << 16);
        // If not valid (no preceding LLI), leave invalid.
      }
      return;
    }

    // Any other instruction that defines a GPR invalidates its tracking.
    const MCInstrDesc &Desc = Info->get(Opc);
    for (unsigned I = 0, E = Desc.getNumDefs(); I < E; ++I) {
      if (Inst.getOperand(I).isReg())
        if (auto Idx = regIndex(Inst.getOperand(I).getReg()))
          GPRValid[*Idx] = false;
    }
  }

  std::optional<uint64_t>
  evaluateMemoryOperandAddress(const MCInst &Inst,
                               const MCSubtargetInfo *STI, uint64_t Addr,
                               uint64_t Size) const override {
    // Annotate LUI with the full 32-bit address reconstructed from
    // the preceding LLI.  At this point updateState() hasn't been
    // called for this instruction yet, so GPRState holds the lo16
    // value from LLI.
    if (Inst.getOpcode() == Penumbra::LUI) {
      if (auto Idx = regIndex(Inst.getOperand(0).getReg())) {
        if (GPRValid[*Idx]) {
          uint64_t Lo = GPRState[*Idx] & 0xFFFF;
          uint64_t Hi = Inst.getOperand(2).getImm();
          return (Hi << 16) | Lo;
        }
      }
    }
    return std::nullopt;
  }

  bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size,
                      uint64_t &Target) const override {
    // Direct branches (B, BEQ..BLE) and direct calls (BL) have an
    // immediate brtarget22 operand.  Indirect branches (JMP, JALR,
    // BRIND) have register operands — isImm() filters those out.
    // The disassembler stores the absolute target address in the
    // operand (computed from PC + sign-extended word offset).
    if ((isBranch(Inst) || isCall(Inst)) && Inst.getOperand(0).isImm()) {
      Target = Inst.getOperand(0).getImm();
      return true;
    }
    return false;
  }
};
} // end anonymous namespace

static MCInstrAnalysis *createPenumbraInstrAnalysis(const MCInstrInfo *Info) {
  return new PenumbraMCInstrAnalysis(Info);
}

static MCInstPrinter *createPenumbraMCInstPrinter(const Triple & /*T*/,
                                                  unsigned SyntaxVariant,
                                                  const MCAsmInfo &MAI,
                                                  const MCInstrInfo &MII,
                                                  const MCRegisterInfo &MRI) {
  return new PenumbraInstPrinter(MAI, MII, MRI);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraTargetMC() {
  Target &T = getThePenumbraTarget();

  RegisterMCAsmInfoFn X(T, createPenumbraMCAsmInfo);

  TargetRegistry::RegisterMCInstrInfo(T, createPenumbraMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createPenumbraMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createPenumbraMCSubtargetInfo);
  TargetRegistry::RegisterMCInstrAnalysis(T, createPenumbraInstrAnalysis);
  TargetRegistry::RegisterMCCodeEmitter(T, createPenumbraMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createPenumbraAsmBackend);
  TargetRegistry::RegisterMCInstPrinter(T, createPenumbraMCInstPrinter);
}

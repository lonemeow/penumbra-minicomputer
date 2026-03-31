//===-- PenumbraInstructionSelector.cpp - Penumbra Instruction Selector ---===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraRegisterBankInfo.h"
#include "PenumbraSubtarget.h"
#include "PenumbraTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelector.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "penumbra-isel"

using namespace llvm;

namespace {

class PenumbraInstructionSelector : public InstructionSelector {
public:
  PenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                               const PenumbraSubtarget &STI,
                               const PenumbraRegisterBankInfo &RBI);

  bool select(MachineInstr &I) override;
  static const char *getName() { return DEBUG_TYPE; }

  // No TableGen-generated match table yet — per-function state is a no-op.
  void setupGeneratedPerFunctionState(MachineFunction &) override {}

private:
  bool selectBinaryALU(MachineInstr &I, unsigned Opc) const;
  bool selectConstant(MachineInstr &I) const;

  const PenumbraInstrInfo &TII;
  const PenumbraRegisterInfo &TRI;
  const PenumbraRegisterBankInfo &RBI;
};

} // end anonymous namespace

PenumbraInstructionSelector::PenumbraInstructionSelector(
    const PenumbraTargetMachine &TM, const PenumbraSubtarget &STI,
    const PenumbraRegisterBankInfo &RBI)
    : InstructionSelector(), TII(*STI.getInstrInfo()),
      TRI(*STI.getRegisterInfo()), RBI(RBI) {}

bool PenumbraInstructionSelector::select(MachineInstr &I) {
  // Non-generic instructions (already selected, e.g. copies inserted by RA
  // or pseudos like ADJCALLSTACKDOWN): accept as-is.
  if (!isPreISelGenericOpcode(I.getOpcode()))
    return true;

  using namespace TargetOpcode;

  switch (I.getOpcode()) {
  // ── Binary ALU ────────────────────────────────────────────────────────────
  // All map to 3-operand machine instructions with a tied dest=src1 constraint.
  // The register allocator inserts a COPY to satisfy the tie in SSA form.
  case G_ADD:  return selectBinaryALU(I, Penumbra::ADD);
  case G_SUB:  return selectBinaryALU(I, Penumbra::SUB);
  case G_AND:  return selectBinaryALU(I, Penumbra::AND);
  case G_OR:   return selectBinaryALU(I, Penumbra::OR);
  case G_XOR:  return selectBinaryALU(I, Penumbra::XOR);
  case G_SHL:  return selectBinaryALU(I, Penumbra::SHL);
  case G_LSHR: return selectBinaryALU(I, Penumbra::SHR);
  case G_ASHR: return selectBinaryALU(I, Penumbra::SAR);

  // ── Constants ─────────────────────────────────────────────────────────────
  case G_CONSTANT: return selectConstant(I);

  default:
    return false;
  }
}

// ── Binary ALU helper ─────────────────────────────────────────────────────────
// G_ADD/G_SUB/... have operands: [def dst, use src1, use src2]
// Penumbra ALU has:              [def $Rd, use $Rd_in(tied), use $Rs]
// We emit them directly; the RA satisfies the tied constraint by copying src1.
bool PenumbraInstructionSelector::selectBinaryALU(MachineInstr &I,
                                                   unsigned Opc) const {
  MachineBasicBlock &MBB = *I.getParent();
  MachineInstr *NewI =
      BuildMI(MBB, I, I.getDebugLoc(), TII.get(Opc))
          .addDef(I.getOperand(0).getReg())  // $Rd  (output)
          .addReg(I.getOperand(1).getReg())  // $Rd_in (tied; RA inserts COPY)
          .addReg(I.getOperand(2).getReg()); // $Rs
  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*NewI, TII, TRI, RBI);
}

// ── G_CONSTANT ────────────────────────────────────────────────────────────────
// Materialize a 32-bit constant into a register.
// Penumbra immediate instructions:
//   LLI  Rd, #imm16 — load zero-extended 16-bit value into Rd
//   LLIS Rd, #imm16 — load sign-extended 16-bit value into Rd
//   LUI  Rd, #imm16 — load upper 16 bits: Rd = (Rd & 0xFFFF) | (imm16 << 16)
//                     (tied: $Rd = $Rd_in — Rd is both input and output)
bool PenumbraInstructionSelector::selectConstant(MachineInstr &I) const {
  Register DstReg = I.getOperand(0).getReg();
  int64_t Val = I.getOperand(1).getCImm()->getSExtValue();
  MachineBasicBlock &MBB = *I.getParent();
  const DebugLoc &DL = I.getDebugLoc();

  // TODO(human): Emit the right instruction sequence to materialize Val into
  // DstReg. Three cases:
  //   1. Val fits in [0, 0xFFFF]   → single LLI  (zero-extend)
  //   2. Val fits in [-32768, -1]  → single LLIS (sign-extend)
  //   3. Full 32-bit               → LLI for low 16 bits, then LUI for high 16
  //      LUI has constraint "$Rd = $Rd_in", so its operands are:
  //      addDef(DstReg), addReg(DstReg), addImm((Val >> 16) & 0xFFFF)
  // Hint: use int64_t comparisons for the range checks.

  if (Val >= 0 && Val <= 0xFFFF) {
    BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
        .addDef(DstReg)
        .addImm(Val);
  } else if (Val < 0 && Val >= -32768) {
    BuildMI(MBB, I, DL, TII.get(Penumbra::LLIS))
        .addDef(DstReg)
        .addImm(Val);
  } else {
    BuildMI(MBB, I, DL, TII.get(Penumbra::LLI))
        .addDef(DstReg)
        .addImm(Val & 0xFFFF);
    BuildMI(MBB, I, DL, TII.get(Penumbra::LUI))
        .addDef(DstReg)
        .addReg(DstReg)
        .addImm((Val >> 16) & 0xFFFF);
  }

  I.eraseFromParent();
  return true;
}

// ── Factory function ──────────────────────────────────────────────────────────

namespace llvm {
InstructionSelector *
createPenumbraInstructionSelector(const PenumbraTargetMachine &TM,
                                  const PenumbraSubtarget &STI,
                                  const PenumbraRegisterBankInfo &RBI) {
  return new PenumbraInstructionSelector(TM, STI, RBI);
}
} // namespace llvm

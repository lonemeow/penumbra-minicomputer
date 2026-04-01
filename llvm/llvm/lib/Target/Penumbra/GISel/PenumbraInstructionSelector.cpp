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
#include "llvm/CodeGenTypes/LowLevelType.h"
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
  bool selectBinaryALU(MachineInstr &I, MachineBasicBlock &MBB,
                       MachineRegisterInfo &MRI, unsigned Opc) const;
  bool selectConstant(MachineInstr &I, MachineBasicBlock &MBB,
                      MachineRegisterInfo &MRI) const;
  bool selectLoad(MachineInstr &I, MachineBasicBlock &MBB,
                  MachineRegisterInfo &MRI) const;
  bool selectStore(MachineInstr &I, MachineBasicBlock &MBB,
                   MachineRegisterInfo &MRI) const;
  bool selectFrameIndex(MachineInstr &I, MachineBasicBlock &MBB,
                        MachineRegisterInfo &MRI) const;

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

  MachineBasicBlock &MBB = *I.getParent();
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();

  using namespace TargetOpcode;

  switch (I.getOpcode()) {
  // ── Binary ALU ────────────────────────────────────────────────────────────
  // All map to 3-operand machine instructions with a tied dest=src1 constraint.
  // The register allocator inserts a COPY to satisfy the tie in SSA form.
  case G_ADD:  return selectBinaryALU(I, MBB, MRI, Penumbra::ADD);
  case G_SUB:  return selectBinaryALU(I, MBB, MRI, Penumbra::SUB);
  case G_AND:  return selectBinaryALU(I, MBB, MRI, Penumbra::AND);
  case G_OR:   return selectBinaryALU(I, MBB, MRI, Penumbra::OR);
  case G_XOR:  return selectBinaryALU(I, MBB, MRI, Penumbra::XOR);
  case G_SHL:  return selectBinaryALU(I, MBB, MRI, Penumbra::SHL);
  case G_LSHR: return selectBinaryALU(I, MBB, MRI, Penumbra::SHR);
  case G_ASHR: return selectBinaryALU(I, MBB, MRI, Penumbra::SAR);

  // ── Constants ─────────────────────────────────────────────────────────────
  case G_CONSTANT: return selectConstant(I, MBB, MRI);

  // ── Memory ────────────────────────────────────────────────────────────────
  case G_LOAD:        return selectLoad(I, MBB, MRI);
  case G_STORE:       return selectStore(I, MBB, MRI);
  case G_FRAME_INDEX: return selectFrameIndex(I, MBB, MRI);

  default:
    return false;
  }
}

// ── Binary ALU helper ─────────────────────────────────────────────────────────
// G_ADD/G_SUB/... have operands: [def dst, use src1, use src2]
// Penumbra ALU has:              [def $Rd, use $Rd_in(tied), use $Rs]
// We emit them directly; the RA satisfies the tied constraint by copying src1.
bool PenumbraInstructionSelector::selectBinaryALU(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI,
                                                   unsigned Opc) const {
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
bool PenumbraInstructionSelector::selectConstant(MachineInstr &I,
                                                   MachineBasicBlock &MBB,
                                                   MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  int64_t Val = I.getOperand(1).getCImm()->getSExtValue();
  const DebugLoc &DL = I.getDebugLoc();

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

// ── Memory helpers ────────────────────────────────────────────────────────────
//
// G_LOAD / G_STORE select into LDW / STW.
//
// Frame-index folding: when the address register is defined by G_FRAME_INDEX,
// we embed the FI directly into the LDW/STW Rb operand instead of materialising
// it as a separate register.  eliminateFrameIndex then patches the FI → R14 and
// fills in the real stack offset, matching the (GPR:$Rb, i32imm:$offset) layout
// of our memory instructions:
//
//   LDW:  (outs GPR:$Rd), (ins GPR:$Rb, i32imm:$offset)
//           operand 0 = dst   operand 1 = Rb (FI here)  operand 2 = offset
//   STW:  (outs),           (ins GPR:$Rd, GPR:$Rb, i32imm:$offset)
//           operand 0 = val   operand 1 = Rb (FI here)  operand 2 = offset

bool PenumbraInstructionSelector::selectLoad(MachineInstr &I,
                                              MachineBasicBlock &MBB,
                                              MachineRegisterInfo &MRI) const {
  Register DstReg  = I.getOperand(0).getReg();
  Register AddrReg = I.getOperand(1).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  if (MRI.getType(DstReg) != LLT::scalar(32))
    return false; // only s32 for now

  auto MIB = BuildMI(MBB, I, DL, TII.get(Penumbra::LDW)).addDef(DstReg);

  MachineInstr *AddrDef = MRI.getVRegDef(AddrReg);
  if (AddrDef && AddrDef->getOpcode() == TargetOpcode::G_FRAME_INDEX)
    MIB.addFrameIndex(AddrDef->getOperand(1).getIndex());
  else
    MIB.addReg(AddrReg);
  MIB.addImm(0);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*MIB, TII, TRI, RBI);
}

bool PenumbraInstructionSelector::selectStore(MachineInstr &I,
                                               MachineBasicBlock &MBB,
                                               MachineRegisterInfo &MRI) const {
  Register ValReg  = I.getOperand(0).getReg();
  Register AddrReg = I.getOperand(1).getReg();
  const DebugLoc &DL = I.getDebugLoc();

  if (MRI.getType(ValReg) != LLT::scalar(32))
    return false;

  auto MIB = BuildMI(MBB, I, DL, TII.get(Penumbra::STW)).addReg(ValReg);

  MachineInstr *AddrDef = MRI.getVRegDef(AddrReg);
  if (AddrDef && AddrDef->getOpcode() == TargetOpcode::G_FRAME_INDEX)
    MIB.addFrameIndex(AddrDef->getOperand(1).getIndex());
  else
    MIB.addReg(AddrReg);
  MIB.addImm(0);

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*MIB, TII, TRI, RBI);
}

bool PenumbraInstructionSelector::selectFrameIndex(MachineInstr &I,
                                                    MachineBasicBlock &MBB,
                                                    MachineRegisterInfo &MRI) const {
  Register DstReg = I.getOperand(0).getReg();
  if (!MRI.use_nodbg_empty(DstReg))
    return false; // address escapes to non-memory use — unsupported
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

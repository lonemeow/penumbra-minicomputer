//===-- PenumbraRegisterBankInfo.cpp ------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraRegisterBankInfo.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterBank.h"
#include "llvm/CodeGen/RegisterBankInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_TARGET_REGBANK_IMPL
#include "PenumbraGenRegisterBank.inc"

using namespace llvm;

// Penumbra has a single register bank (GPR) covering all 32-bit values.
// The ValueMapping table provides a GPR entry that getInstrMapping() points
// into for each register operand.

namespace llvm {
namespace Penumbra {

enum PartialMappingIdx {
  PMI_GPR32,
};

const RegisterBankInfo::PartialMapping PartMappings[] = {
    // 32-bit value in the GPR bank
    {0, 32, GPRRegBank},
};

// ValueMappings[0] is the invalid sentinel.
// ValueMappings[1..3] are three consecutive GPR entries — one per operand.
// Instructions with fewer operands use only the first N entries.
enum ValueMappingIdx {
  InvalidIdx = 0,
  GPR3OpsIdx = 1,
};

const RegisterBankInfo::ValueMapping ValueMappings[] = {
    // invalid
    {nullptr, 0},
    // 3-operand GPR mapping (dst, src1, src2 — or fewer)
    {&PartMappings[PMI_GPR32], 1},
    {&PartMappings[PMI_GPR32], 1},
    {&PartMappings[PMI_GPR32], 1},
};

} // namespace Penumbra
} // namespace llvm

PenumbraRegisterBankInfo::PenumbraRegisterBankInfo(const TargetRegisterInfo &TRI)
    : PenumbraGenRegisterBankInfo() {}

const RegisterBankInfo::InstructionMapping &
PenumbraRegisterBankInfo::getInstrMapping(const MachineInstr &MI) const {
  unsigned Opc = MI.getOpcode();

  // Non-generic instructions and G_PHI (which is copy-like) are handled by
  // the generic infrastructure — it maps just the def from register class info.
  if (!isPreISelGenericOpcode(Opc) || Opc == TargetOpcode::G_PHI) {
    const InstructionMapping &Mapping = getInstrMappingImpl(MI);
    if (Mapping.isValid())
      return Mapping;
  }

  // Penumbra has a single register bank (GPR). Map every register operand to
  // GPR and skip non-register operands (immediates, predicates, MBBs).
  unsigned NumOperands = MI.getNumOperands();
  SmallVector<const ValueMapping *, 4> OpdsMapping(NumOperands);
  const auto *GPRMapping = &Penumbra::ValueMappings[Penumbra::GPR3OpsIdx];

  for (unsigned Idx = 0; Idx < NumOperands; ++Idx) {
    auto &MO = MI.getOperand(Idx);
    if (!MO.isReg() || !MO.getReg())
      continue;
    OpdsMapping[Idx] = GPRMapping;
  }

  return getInstructionMapping(DefaultMappingID, /*Cost=*/1,
                               getOperandsMapping(OpdsMapping), NumOperands);
}

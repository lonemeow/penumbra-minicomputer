//===-- PenumbraRegisterBankInfo.cpp ------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraRegisterBankInfo.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterBank.h"
#include "llvm/CodeGen/RegisterBankInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_TARGET_REGBANK_IMPL
#include "PenumbraGenRegisterBank.inc"

using namespace llvm;

// Penumbra has a single register bank (GPR) covering all 32-bit values.
// We define one PartialMapping (32-bit GPR) and a ValueMapping table with
// entries for up to 3 operands — enough for any binary op (dst, src1, src2).
//
// G_CONSTANT and G_FRAME_INDEX have only one register operand (the output);
// the second "operand" is an immediate, represented as nullptr in the mapping.

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

  // Non-generic (post-isel) instructions: use the default mapping from
  // register class information already on the instruction.
  if (!isPreISelGenericOpcode(Opc)) {
    const InstructionMapping &Mapping = getInstrMappingImpl(MI);
    if (Mapping.isValid())
      return Mapping;
  }

  using namespace TargetOpcode;

  unsigned NumOperands = MI.getNumOperands();
  const auto *OpMapping = &Penumbra::ValueMappings[Penumbra::GPR3OpsIdx];

  switch (Opc) {
  // Binary ALU: dst, src1, src2 — all GPR
  case G_ADD: case G_SUB:
  case G_AND: case G_OR: case G_XOR:
  case G_SHL: case G_LSHR: case G_ASHR:
    break;

  // Unary: dst, src — uses first 2 entries of GPR3OpsIdx
  case G_ZEXT: case G_SEXT: case G_ANYEXT:
  case G_TRUNC:
    break;

  // Memory: G_LOAD (dst, ptr), G_STORE (src, ptr), extend-loads
  case G_LOAD: case G_STORE:
  case G_ZEXTLOAD: case G_SEXTLOAD:
    break;

  // Single-output, non-register input: map output to GPR, nullptr for rest
  case G_CONSTANT:
  case G_FRAME_INDEX:
  case G_GLOBAL_VALUE:
    OpMapping = getOperandsMapping(
        {&Penumbra::ValueMappings[Penumbra::GPR3OpsIdx], nullptr});
    break;

  default:
    return getInvalidInstructionMapping();
  }

  return getInstructionMapping(DefaultMappingID, /*Cost=*/1,
                               OpMapping, NumOperands);
}

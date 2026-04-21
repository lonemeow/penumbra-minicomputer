//===-- PenumbraDisassembler.cpp - Disassembler for Penumbra --------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraDisassembler.h"

#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"

#define DEBUG_TYPE "penumbra-disassembler"

using namespace llvm;
using namespace llvm::MCD;

typedef MCDisassembler::DecodeStatus DecodeStatus;

static MCDisassembler *createPenumbraDisassembler(const Target & /*T*/,
                                                  const MCSubtargetInfo &STI,
                                                  MCContext &Ctx) {
  return new PenumbraDisassembler(STI, Ctx);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getThePenumbraTarget(),
                                         createPenumbraDisassembler);
}

//===----------------------------------------------------------------------===//
// Register decoder
//===----------------------------------------------------------------------===//

// Maps 4-bit hardware encoding to LLVM register number.
static const unsigned GPRDecoderTable[] = {
    Penumbra::R0,  Penumbra::R1,  Penumbra::R2,  Penumbra::R3,
    Penumbra::R4,  Penumbra::R5,  Penumbra::R6,  Penumbra::R7,
    Penumbra::R8,  Penumbra::R9,  Penumbra::R10, Penumbra::R11,
    Penumbra::R12, Penumbra::R13, Penumbra::R14, Penumbra::R15,
};

static DecodeStatus DecodeGPRRegisterClass(MCInst &Inst, unsigned RegNo,
                                           uint64_t /*Address*/,
                                           const MCDisassembler * /*Decoder*/) {
  if (RegNo > 15)
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createReg(GPRDecoderTable[RegNo]));
  return MCDisassembler::Success;
}

//===----------------------------------------------------------------------===//
// Operand decoders
//===----------------------------------------------------------------------===//

// Decode 22-bit signed word offset → absolute target address.
static DecodeStatus decodeBranchTarget(MCInst &Inst, unsigned FieldVal,
                                       uint64_t Address,
                                       const MCDisassembler *Decoder) {
  int64_t Target = Address + (SignExtend64<22>(FieldVal) << 2);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Target, Address, true, 0, 4, 0)) {
    Inst.addOperand(MCOperand::createImm(Target));
  }
  return MCDisassembler::Success;
}

// 16-bit unsigned immediate (LLI, LUI, ADDi, SUBi, shifts, etc.).
static DecodeStatus decodeImm16(MCInst &Inst, unsigned FieldVal,
                                uint64_t /*Address*/,
                                const MCDisassembler * /*Decoder*/) {
  Inst.addOperand(MCOperand::createImm(FieldVal));
  return MCDisassembler::Success;
}

// 16-bit signed immediate (LLIS — hardware sign-extends to 32-bit).
static DecodeStatus decodeSimm16(MCInst &Inst, unsigned FieldVal,
                                 uint64_t /*Address*/,
                                 const MCDisassembler * /*Decoder*/) {
  Inst.addOperand(MCOperand::createImm(SignExtend32<16>(FieldVal)));
  return MCDisassembler::Success;
}

// 16-bit signed memory offset (extracted from bits [17:2]).
static DecodeStatus decodeMemOffset16(MCInst &Inst, unsigned FieldVal,
                                      uint64_t /*Address*/,
                                      const MCDisassembler * /*Decoder*/) {
  Inst.addOperand(MCOperand::createImm(SignExtend32<16>(FieldVal)));
  return MCDisassembler::Success;
}

//===----------------------------------------------------------------------===//
// Auto-generated decoder tables
//===----------------------------------------------------------------------===//

#include "PenumbraGenDisassemblerTables.inc"

//===----------------------------------------------------------------------===//
// Instruction reading and dispatch
//===----------------------------------------------------------------------===//

DecodeStatus
PenumbraDisassembler::getInstruction(MCInst &Instr, uint64_t &Size,
                                     ArrayRef<uint8_t> Bytes, uint64_t Address,
                                     raw_ostream & /*CStream*/) const {
  // All Penumbra instructions are 32-bit fixed width.
  if (Bytes.size() < 4) {
    Size = 0;
    return MCDisassembler::Fail;
  }

  // Little-endian 32-bit read.
  uint32_t Insn = (Bytes[3] << 24) | (Bytes[2] << 16) |
                  (Bytes[1] << 8) | Bytes[0];

  DecodeStatus Result =
      decodeInstruction(DecoderTablePenumbra32, Instr, Insn, Address, this, STI);

  if (Result != MCDisassembler::Fail) {
    Size = 4;
    return Result;
  }

  Size = 4; // Always consume 4 bytes even on failure (fixed-width ISA).
  return MCDisassembler::Fail;
}

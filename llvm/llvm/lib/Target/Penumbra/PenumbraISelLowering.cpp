//===-- PenumbraISelLowering.cpp - Penumbra DAG Lowering -------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraISelLowering.h"
#include "PenumbraSubtarget.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/TargetLowering.h"

using namespace llvm;

PenumbraISelLowering::PenumbraISelLowering(const TargetMachine &TM,
                                            const PenumbraSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &Penumbra::GPR_AllocatableRegClass);

  // No multiply/divide hardware — expand to libcalls
  setOperationAction(ISD::MUL,        MVT::i32, Expand);
  setOperationAction(ISD::MULHS,      MVT::i32, Expand);
  setOperationAction(ISD::MULHU,      MVT::i32, Expand);
  setOperationAction(ISD::SMUL_LOHI,  MVT::i32, Expand);
  setOperationAction(ISD::UMUL_LOHI,  MVT::i32, Expand);
  setOperationAction(ISD::SDIV,       MVT::i32, Expand);
  setOperationAction(ISD::UDIV,       MVT::i32, Expand);
  setOperationAction(ISD::SREM,       MVT::i32, Expand);
  setOperationAction(ISD::UREM,       MVT::i32, Expand);
  setOperationAction(ISD::SDIVREM,    MVT::i32, Expand);
  setOperationAction(ISD::UDIVREM,    MVT::i32, Expand);

  // No bit-manipulation instructions
  setOperationAction(ISD::BSWAP,   MVT::i32, Expand);
  setOperationAction(ISD::ROTL,    MVT::i32, Expand);
  setOperationAction(ISD::ROTR,    MVT::i32, Expand);
  setOperationAction(ISD::CTLZ,    MVT::i32, Expand);
  setOperationAction(ISD::CTTZ,    MVT::i32, Expand);
  setOperationAction(ISD::CTPOP,   MVT::i32, Expand);

  computeRegisterProperties(STI.getRegisterInfo());
}

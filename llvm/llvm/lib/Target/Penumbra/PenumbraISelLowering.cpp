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

// TableGen-generated CC assignment functions (CC_Penumbra, RetCC_Penumbra).
// Must be included after `using namespace llvm` — the generated code uses
// MVT, CCValAssign, CCState etc. without namespace qualification.
#include "PenumbraGenCallingConv.inc"

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

CCAssignFn *PenumbraISelLowering::getCCAssignFn(CallingConv::ID CC,
                                                 bool Return,
                                                 bool IsVarArg) const {
  return Return ? RetCC_Penumbra : CC_Penumbra;
}

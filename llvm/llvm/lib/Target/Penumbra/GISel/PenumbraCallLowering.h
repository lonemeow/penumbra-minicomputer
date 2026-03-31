//===-- PenumbraCallLowering.h - Call lowering ------------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRACALLLOWERING_H
#define LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRACALLLOWERING_H

#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"

namespace llvm {

class PenumbraISelLowering;

class PenumbraCallLowering : public CallLowering {
public:
  PenumbraCallLowering(const PenumbraISelLowering &TLI);

  bool lowerReturn(MachineIRBuilder &MIRBuilder, const Value *Val,
                   ArrayRef<Register> VRegs, FunctionLoweringInfo &FLI,
                   Register SwiftErrorVReg) const override;

  bool lowerFormalArguments(MachineIRBuilder &MIRBuilder, const Function &F,
                            ArrayRef<ArrayRef<Register>> VRegs,
                            FunctionLoweringInfo &FLI) const override;

  bool lowerCall(MachineIRBuilder &MIRBuilder,
                 CallLoweringInfo &Info) const override;
};

// Handles incoming values (function arguments or call return values):
// marks live-ins, copies physregs → vregs, loads from stack.
struct PenumbraIncomingValueHandler : public CallLowering::IncomingValueHandler {
  PenumbraIncomingValueHandler(MachineIRBuilder &MIRBuilder,
                               MachineRegisterInfo &MRI)
      : CallLowering::IncomingValueHandler(MIRBuilder, MRI), StackUsed(0) {}

  uint64_t StackUsed;

private:
  void assignValueToReg(Register ValVReg, Register PhysReg,
                        const CCValAssign &VA) override;
  void assignValueToAddress(Register ValVReg, Register Addr, LLT MemTy,
                            const MachinePointerInfo &MPO,
                            const CCValAssign &VA) override;
  Register getStackAddress(uint64_t Size, int64_t Offset,
                           MachinePointerInfo &MPO,
                           ISD::ArgFlagsTy Flags) override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_GISEL_PENUMBRACALLLOWERING_H

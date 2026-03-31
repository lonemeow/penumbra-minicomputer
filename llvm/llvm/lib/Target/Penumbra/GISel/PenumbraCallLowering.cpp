//===-- PenumbraCallLowering.cpp - Call lowering -------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraCallLowering.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraISelLowering.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"

using namespace llvm;

// Outgoing handler used only for building the RET instruction: copies a
// vreg → physical return register and marks it as an implicit use on the RET.
namespace {
struct PenumbraReturnHandler : public CallLowering::OutgoingValueHandler {
  PenumbraReturnHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                        MachineInstrBuilder &MIB)
      : OutgoingValueHandler(MIRBuilder, MRI), MIB(MIB) {}

  void assignValueToReg(Register ValVReg, Register PhysReg,
                        const CCValAssign &VA) override {
    MIB.addUse(PhysReg, RegState::Implicit);
    MIRBuilder.buildCopy(PhysReg, extendRegister(ValVReg, VA));
  }

  // Stack returns not supported (RetCC_Penumbra only assigns to R1).
  void assignValueToAddress(Register, Register, LLT, const MachinePointerInfo &,
                            const CCValAssign &) override {}
  Register getStackAddress(uint64_t, int64_t, MachinePointerInfo &,
                           ISD::ArgFlagsTy) override {
    return {};
  }

  MachineInstrBuilder &MIB;
};
} // namespace

PenumbraCallLowering::PenumbraCallLowering(const PenumbraISelLowering &TLI)
    : CallLowering(&TLI) {}

bool PenumbraCallLowering::lowerReturn(MachineIRBuilder &MIRBuilder,
                                       const Value *Val,
                                       ArrayRef<Register> VRegs,
                                       FunctionLoweringInfo &FLI,
                                       Register SwiftErrorVReg) const {
  auto MIB = MIRBuilder.buildInstrNoInsert(Penumbra::RET);
  MachineFunction &MF = MIRBuilder.getMF();
  const Function &F = MF.getFunction();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const PenumbraISelLowering &TLI = *getTLI<PenumbraISelLowering>();

  if (!VRegs.empty()) {
    SmallVector<ArgInfo, 4> SplitArgs;
    ArgInfo OrigArg{VRegs, Val->getType(), 0};
    setArgFlags(OrigArg, AttributeList::ReturnIndex, F.getDataLayout(), F);
    splitToValueTypes(OrigArg, SplitArgs, F.getDataLayout(), F.getCallingConv());

    CCAssignFn *AssignFn =
        TLI.getCCAssignFn(F.getCallingConv(), /*Return=*/true, F.isVarArg());
    OutgoingValueAssigner ArgAssigner(AssignFn);
    PenumbraReturnHandler ArgHandler(MIRBuilder, MRI, MIB);
    if (!determineAndHandleAssignments(ArgHandler, ArgAssigner, SplitArgs,
                                       MIRBuilder, F.getCallingConv(),
                                       F.isVarArg()))
      return false;
  }

  MIRBuilder.insertInstr(MIB);
  return true;
}

bool PenumbraCallLowering::lowerFormalArguments(
    MachineIRBuilder &MIRBuilder, const Function &F,
    ArrayRef<ArrayRef<Register>> VRegs, FunctionLoweringInfo &FLI) const {
  MachineFunction &MF = MIRBuilder.getMF();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const auto &DL = F.getDataLayout();

  SmallVector<ArgInfo, 8> SplitArgs;
  unsigned I = 0;
  for (const auto &Arg : F.args()) {
    ArgInfo OrigArg{VRegs[I], Arg.getType(), I};
    setArgFlags(OrigArg, I + AttributeList::FirstArgIndex, DL, F);
    splitToValueTypes(OrigArg, SplitArgs, DL, F.getCallingConv());
    ++I;
  }

  const PenumbraISelLowering &TLI = *getTLI<PenumbraISelLowering>();
  CCAssignFn *AssignFn =
      TLI.getCCAssignFn(F.getCallingConv(), /*Return=*/false, F.isVarArg());
  IncomingValueAssigner ArgAssigner(AssignFn);
  PenumbraIncomingValueHandler ArgHandler(MIRBuilder, MRI);
  return determineAndHandleAssignments(ArgHandler, ArgAssigner, SplitArgs,
                                       MIRBuilder, F.getCallingConv(),
                                       F.isVarArg());
}

bool PenumbraCallLowering::lowerCall(MachineIRBuilder &MIRBuilder,
                                     CallLoweringInfo &Info) const {
  // Calls are not needed for the first milestone (leaf functions only).
  return false;
}

// ── IncomingValueHandler ──────────────────────────────────────────────────────

void PenumbraIncomingValueHandler::assignValueToReg(Register ValVReg,
                                                    Register PhysReg,
                                                    const CCValAssign &VA) {
  MIRBuilder.getMRI()->addLiveIn(PhysReg);
  MIRBuilder.getMBB().addLiveIn(PhysReg);
  IncomingValueHandler::assignValueToReg(ValVReg, PhysReg, VA);
}

void PenumbraIncomingValueHandler::assignValueToAddress(
    Register ValVReg, Register Addr, LLT MemTy, const MachinePointerInfo &MPO,
    const CCValAssign &VA) {
  MachineFunction &MF = MIRBuilder.getMF();
  auto *MMO = MF.getMachineMemOperand(MPO, MachineMemOperand::MOLoad, MemTy,
                                      inferAlignFromPtrInfo(MF, MPO));
  MIRBuilder.buildLoad(ValVReg, Addr, *MMO);
}

Register PenumbraIncomingValueHandler::getStackAddress(uint64_t Size,
                                                       int64_t Offset,
                                                       MachinePointerInfo &MPO,
                                                       ISD::ArgFlagsTy Flags) {
  auto &MFI = MIRBuilder.getMF().getFrameInfo();
  int FI = MFI.CreateFixedObject(Size, Offset, !Flags.isByVal());
  MPO = MachinePointerInfo::getFixedStack(MIRBuilder.getMF(), FI);
  MachineInstrBuilder AddrReg = MIRBuilder.buildFrameIndex(
      LLT::pointer(0, 32), FI);
  StackUsed = std::max(StackUsed, Size + (uint64_t)Offset);
  return AddrReg.getReg(0);
}

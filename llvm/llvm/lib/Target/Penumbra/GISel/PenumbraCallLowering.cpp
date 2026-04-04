//===-- PenumbraCallLowering.cpp - Call lowering -------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraCallLowering.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraISelLowering.h"
#include "PenumbraMachineFunctionInfo.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"

using namespace llvm;

// Outgoing handler for both RET and BL: copies a vreg → physical register
// and marks the register as an implicit use on the instruction being built.
namespace {
struct PenumbraOutgoingValueHandler : public CallLowering::OutgoingValueHandler {
  PenumbraOutgoingValueHandler(MachineIRBuilder &MIRBuilder,
                               MachineRegisterInfo &MRI,
                               MachineInstrBuilder &MIB)
      : OutgoingValueHandler(MIRBuilder, MRI), MIB(MIB) {}

  void assignValueToReg(Register ValVReg, Register PhysReg,
                        const CCValAssign &VA) override {
    MIB.addUse(PhysReg, RegState::Implicit);
    MIRBuilder.buildCopy(PhysReg, extendRegister(ValVReg, VA));
  }

  void assignValueToAddress(Register ValVReg, Register Addr, LLT MemTy,
                            const MachinePointerInfo &MPO,
                            const CCValAssign &VA) override {
    MachineFunction &MF = MIRBuilder.getMF();
    auto *MMO = MF.getMachineMemOperand(MPO, MachineMemOperand::MOStore, MemTy,
                                        inferAlignFromPtrInfo(MF, MPO));
    Register ExtReg = extendRegister(ValVReg, VA);
    MIRBuilder.buildStore(ExtReg, Addr, *MMO);
  }

  Register getStackAddress(uint64_t MemSize, int64_t Offset,
                           MachinePointerInfo &MPO,
                           ISD::ArgFlagsTy Flags) override {
    LLT p0 = LLT::pointer(0, 32);
    LLT s32 = LLT::scalar(32);
    auto SPReg = MIRBuilder.buildCopy(p0, Register(Penumbra::R14));
    auto OffsetReg = MIRBuilder.buildConstant(s32, Offset);
    auto AddrReg = MIRBuilder.buildPtrAdd(p0, SPReg, OffsetReg);
    MPO = MachinePointerInfo::getStack(MIRBuilder.getMF(), Offset);
    return AddrReg.getReg(0);
  }

  MachineInstrBuilder &MIB;
};

// Incoming handler for call return values: marks physical registers as
// implicit defs on the BL instruction.
struct PenumbraCallReturnHandler : public CallLowering::IncomingValueHandler {
  PenumbraCallReturnHandler(MachineIRBuilder &MIRBuilder,
                            MachineRegisterInfo &MRI,
                            MachineInstrBuilder &MIB)
      : IncomingValueHandler(MIRBuilder, MRI), MIB(MIB) {}

  void assignValueToReg(Register ValVReg, Register PhysReg,
                        const CCValAssign &VA) override {
    MIB.addDef(PhysReg, RegState::Implicit);
    MIRBuilder.buildCopy(ValVReg, PhysReg);
  }

  void assignValueToAddress(Register ValVReg, Register Addr, LLT MemTy,
                            const MachinePointerInfo &MPO,
                            const CCValAssign &VA) override {}
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
    PenumbraOutgoingValueHandler ArgHandler(MIRBuilder, MRI, MIB);
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
  if (!determineAndHandleAssignments(ArgHandler, ArgAssigner, SplitArgs,
                                     MIRBuilder, F.getCallingConv(),
                                     F.isVarArg()))
    return false;

  // Variadic functions: spill all argument registers (R1-R4) to the stack
  // so that va_arg can walk a contiguous region covering both register-passed
  // and stack-passed arguments.  The ABI requires this save area to sit
  // immediately below the caller's stack-passed arguments.
  if (F.isVarArg()) {
    static const MCPhysReg ArgRegs[] = {Penumbra::R1, Penumbra::R2,
                                        Penumbra::R3, Penumbra::R4};
    const unsigned NumArgRegs = std::size(ArgRegs);
    // How many regs were consumed by named args?
    unsigned NumNamed = SplitArgs.size();
    if (NumNamed > NumArgRegs)
      NumNamed = NumArgRegs;

    // Offset from SP where the save area starts.  Named register args were
    // already assigned by CC_Penumbra.  The save area covers all 4 register
    // slots (16 bytes), placed at a fixed negative offset from the incoming
    // SP so that it is contiguous with any stack-passed arguments.
    unsigned SaveSize = NumArgRegs * 4;  // 16 bytes for R1-R4
    int64_t SaveOffset = -(int64_t)SaveSize;

    MachineFrameInfo &MFI = MF.getFrameInfo();

    // Spill each argument register to its slot.
    LLT s32 = LLT::scalar(32);
    LLT p0 = LLT::pointer(0, 32);
    for (unsigned i = 0; i < NumArgRegs; ++i) {
      int SlotFI = MFI.CreateFixedObject(4, SaveOffset + i * 4,
                                         /*IsSpillSlot=*/true);
      auto FINReg = MIRBuilder.buildFrameIndex(p0, SlotFI);
      // If this register carries a named arg it was already live-in'd above.
      // For unused arg regs we need to add them as live-ins.
      if (i >= NumNamed) {
        MIRBuilder.getMRI()->addLiveIn(ArgRegs[i]);
        MIRBuilder.getMBB().addLiveIn(ArgRegs[i]);
      }
      Register CopyReg = MRI.createGenericVirtualRegister(s32);
      MIRBuilder.buildCopy(CopyReg, Register(ArgRegs[i]));
      MachinePointerInfo MPO =
          MachinePointerInfo::getFixedStack(MF, SlotFI);
      auto *MMO = MF.getMachineMemOperand(MPO, MachineMemOperand::MOStore,
                                          s32, Align(4));
      MIRBuilder.buildStore(CopyReg, FINReg, *MMO);
    }

    // Record the frame index for the start of the save area.  va_start
    // will use this to initialise the va_list pointer.  The va_list points
    // past the named args: if 2 named args exist, va_list = &save[2].
    auto *FuncInfo = MF.getInfo<PenumbraMachineFunctionInfo>();
    // VarArgs start at the first anonymous slot.
    int VaFI = MFI.CreateFixedObject(4, SaveOffset + NumNamed * 4,
                                     /*IsSpillSlot=*/false);
    FuncInfo->setVarArgsFrameIndex(VaFI);
  }

  return true;
}

bool PenumbraCallLowering::lowerCall(MachineIRBuilder &MIRBuilder,
                                     CallLoweringInfo &Info) const {
  MachineFunction &MF = MIRBuilder.getMF();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const DataLayout &DL = MF.getDataLayout();
  const PenumbraISelLowering &TLI = *getTLI<PenumbraISelLowering>();
  CallingConv::ID CC = Info.CallConv;

  // TODO: tail calls
  Info.IsTailCall = false;

  // ADJCALLSTACKDOWN — reserve space for outgoing arguments on the stack.
  // The actual amount is filled in after argument assignment.
  MachineInstrBuilder CallSeqStart =
      MIRBuilder.buildInstr(Penumbra::ADJCALLSTACKDOWN);

  // Split outgoing arguments according to the calling convention.
  SmallVector<ArgInfo, 8> SplitArgs;
  for (auto &AInfo : Info.OrigArgs)
    splitToValueTypes(AInfo, SplitArgs, DL, CC);

  // Build the call instruction.
  // Direct calls (symbol target): BL (branch and link, Format B, PC-relative).
  // Indirect calls (register target): JALR (jump and link register, Format L).
  MachineInstrBuilder MIB;
  if (Info.Callee.isReg()) {
    MIB = MIRBuilder.buildInstrNoInsert(Penumbra::JALR).add(Info.Callee);
    MRI.setRegClass(Info.Callee.getReg(), &Penumbra::GPRRegClass);
  } else
    MIB = MIRBuilder.buildInstrNoInsert(Penumbra::BL).add(Info.Callee);

  // Add the call-preserved register mask so the register allocator knows
  // which registers survive across the call.
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  MIB.addRegMask(TRI->getCallPreservedMask(MF, CC));

  // Assign outgoing arguments to physical registers / stack slots.
  CCAssignFn *AssignFn = TLI.getCCAssignFn(CC, /*Return=*/false, Info.IsVarArg);
  OutgoingValueAssigner ArgAssigner(AssignFn);
  PenumbraOutgoingValueHandler ArgHandler(MIRBuilder, MRI, MIB);
  if (!determineAndHandleAssignments(ArgHandler, ArgAssigner, SplitArgs,
                                     MIRBuilder, CC, Info.IsVarArg))
    return false;

  // Now insert the call.
  MIRBuilder.insertInstr(MIB);

  // Fill in the stack adjustment amounts.
  uint64_t StackSize = ArgAssigner.StackSize;
  CallSeqStart.addImm(StackSize).addImm(0);
  MIRBuilder.buildInstr(Penumbra::ADJCALLSTACKUP).addImm(StackSize).addImm(0);

  // Collect the return value if the call is non-void.
  if (!Info.OrigRet.Ty->isVoidTy()) {
    SmallVector<ArgInfo, 4> SplitRetArgs;
    splitToValueTypes(Info.OrigRet, SplitRetArgs, DL, CC);

    CCAssignFn *RetAssignFn = TLI.getCCAssignFn(CC, /*Return=*/true,
                                                  Info.IsVarArg);
    IncomingValueAssigner RetAssigner(RetAssignFn);
    PenumbraCallReturnHandler RetHandler(MIRBuilder, MRI, MIB);
    if (!determineAndHandleAssignments(RetHandler, RetAssigner, SplitRetArgs,
                                       MIRBuilder, CC, Info.IsVarArg))
      return false;
  }

  return true;
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

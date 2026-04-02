//===-- PenumbraLegalizerInfo.cpp ---------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraLegalizerInfo.h"
#include "PenumbraMachineFunctionInfo.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"

using namespace llvm;

PenumbraLegalizerInfo::PenumbraLegalizerInfo(const PenumbraSubtarget &ST) {
  using namespace TargetOpcode;

  const LLT s8  = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT p0  = LLT::pointer(0, 32);

  getActionDefinitionsBuilder({G_ADD, G_SUB, G_AND, G_OR, G_XOR, G_SHL, G_LSHR, G_ASHR})
      .legalFor({s32}) 
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder({G_FRAME_INDEX, G_GLOBAL_VALUE})
      .legalFor({p0});

  getActionDefinitionsBuilder({G_STORE, G_LOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s32, 4},  // LDW/STW
        {s32, p0, s16, 2},  // STH (store) / plain LDH (zero-extend, handled by G_ZEXTLOAD too)
        {s32, p0, s8,  1},  // STB / plain LDB
        {p0,  p0, s32, 4},  // pointer load/store
      })
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder({G_SEXTLOAD, G_ZEXTLOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s16, 2},
        {s32, p0, s8,  1},
      });

  getActionDefinitionsBuilder({G_PTR_ADD, G_PTRMASK})
      .legalFor({{p0, s32}});

  // Pointer/integer casts: no-op on Penumbra (pointers = 32-bit integers).
  // G_INTTOPTR: type0=p0 (result), type1=s32 (source).
  // G_PTRTOINT: type0=s32 (result), type1=p0 (source).
  getActionDefinitionsBuilder({G_INTTOPTR, G_PTRTOINT})
      .legalFor({{p0, s32}, {s32, p0}});

  // Comparisons: G_ICMP produces s1 result, compares s32 operands.
  const LLT s1 = LLT::scalar(1);
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{s1, s32}, {s1, p0}})
      .clampScalar(1, s32, s32);

  // PHI nodes at control-flow joins.
  getActionDefinitionsBuilder(G_PHI)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  // Branches.
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({s1});

  // Jump tables: G_JUMP_TABLE materialises table address, G_BRJT branches.
  getActionDefinitionsBuilder(G_JUMP_TABLE).legalFor({p0});
  getActionDefinitionsBuilder(G_BRJT).alwaysLegal();

  // Select (ternary): result s32/p0, condition s1.
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{s32, s1}, {p0, s1}})
      .clampScalar(0, s32, s32);

  // Extensions: handled in instruction selection (AND for zext, SHL+SAR for sext).
  getActionDefinitionsBuilder({G_ZEXT, G_SEXT, G_ANYEXT})
      .legalForCartesianProduct({s8, s16, s32}, {s1, s8, s16});

  // Division/remainder: no hardware support — lower to libcalls
  // (__udivsi3, __umodsi3, __divsi3, __modsi3 in libc.c).
  getActionDefinitionsBuilder({G_UDIV, G_UREM, G_SDIV, G_SREM})
      .libcallFor({s32})
      .clampScalar(0, s32, s32);

  // Multiplication: no hardware support yet — lower to libcall (__mulsi3).
  getActionDefinitionsBuilder(G_MUL)
      .libcallFor({s32})
      .clampScalar(0, s32, s32);

  // SEXT_INREG: lowered by framework to SHL+ASHR (our shift constant folding
  // then selects these to SHLi+SARi).
  getActionDefinitionsBuilder(G_SEXT_INREG).lower();

  // Truncation: no-op at the register level (just use the low bits).
  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{s1, s32}, {s8, s32}, {s16, s32}});

  // G_ABS: the optimizer generates this at -O1+ for signed division.
  // Lower to the generic SELECT expansion (icmp + negate + select).
  getActionDefinitionsBuilder(G_ABS).lower();

  // G_FREEZE: converts potentially-poison values to well-defined ones.
  // At -O1+ the optimizer inserts these around division and other ops.
  // No-op on Penumbra — just pass the value through.
  getActionDefinitionsBuilder(G_FREEZE)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  // Varargs: G_VASTART is custom-lowered to store the save area address
  // into the va_list pointer.  G_VAARG is lowered generically (pointer
  // bump + load).
  getActionDefinitionsBuilder(G_VASTART).customFor({p0});
  getActionDefinitionsBuilder(G_VAARG)
      .clampScalar(0, s32, s32)
      .lowerForCartesianProduct({s32, p0}, {p0});

  getLegacyLegalizerInfo().computeTables();
}

bool PenumbraLegalizerInfo::legalizeCustom(
    LegalizerHelper &Helper, MachineInstr &MI,
    LostDebugLocObserver &LocObserver) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;

  switch (MI.getOpcode()) {
  default:
    return false;
  case TargetOpcode::G_VASTART: {
    // G_VASTART stores the address of the first anonymous argument into
    // the va_list pointer (operand 0).  The frame index was recorded by
    // lowerFormalArguments in PenumbraMachineFunctionInfo.
    MachineFunction *MF = MI.getParent()->getParent();
    auto *FuncInfo = MF->getInfo<PenumbraMachineFunctionInfo>();
    int FI = FuncInfo->getVarArgsFrameIndex();
    LLT AddrTy = MIRBuilder.getMRI()->getType(MI.getOperand(0).getReg());
    auto FINAddr = MIRBuilder.buildFrameIndex(AddrTy, FI);
    assert(MI.hasOneMemOperand());
    MIRBuilder.buildStore(FINAddr, MI.getOperand(0).getReg(),
                          **MI.memoperands_begin());
    MI.eraseFromParent();
    return true;
  }
  }
}

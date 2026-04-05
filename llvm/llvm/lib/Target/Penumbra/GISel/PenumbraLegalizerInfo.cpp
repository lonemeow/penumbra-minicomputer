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
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

PenumbraLegalizerInfo::PenumbraLegalizerInfo(const PenumbraSubtarget &ST) {
  using namespace TargetOpcode;
  using namespace LegalityPredicates;
  using namespace LegalizeMutations;

  const LLT s1  = LLT::scalar(1);
  const LLT s8  = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT p0  = LLT::pointer(0, 32);

  getActionDefinitionsBuilder({G_ADD, G_SUB, G_AND, G_OR, G_XOR})
      .legalFor({s32})
      .clampScalar(0, s32, s32);

  // Shifts: clamp both the value (type 0) and shift amount (type 1) to s32.
  // Without clamping type 1, i64 narrowing can produce s64 shift amounts.
  getActionDefinitionsBuilder({G_SHL, G_LSHR, G_ASHR})
      .legalFor({{s32, s32}})
      .clampScalar(0, s32, s32)
      .clampScalar(1, s32, s32);

  // Add/sub with overflow and carry: produced by i64 narrowing.
  // All lowered to basic ADD/SUB + ICMP sequences. The hardware has ADC/SBC
  // instructions but using them requires SR flag management that GlobalISel
  // doesn't handle well at -O0. A future peephole pass can fuse
  // ADD+compare+ADC chains into ADD+ADC.
  getActionDefinitionsBuilder({G_UADDO, G_USUBO, G_UADDE, G_USUBE})
      .lowerFor({{s32, s1}});

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
      .widenScalarToNextPow2(0, /* MinSize = */ 8)
      .lowerIfMemSizeNotByteSizePow2()
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
  // Sub-word results (e.g. (char)(uintptr_t)ptr) are widened to s32
  // first, then truncated by G_TRUNC.
  getActionDefinitionsBuilder({G_INTTOPTR, G_PTRTOINT})
      .legalFor({{p0, s32}, {s32, p0}})
      .minScalar(0, s32)
      .minScalar(1, s32);

  // Comparisons: G_ICMP produces s1 result, compares s32 operands.
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{s1, s32}, {s1, p0}})
      .clampScalar(1, s32, s32);

  // Undefined value: used by the optimizer for uninitialized variables.
  getActionDefinitionsBuilder(G_IMPLICIT_DEF)
      .legalFor({s32, s64, p0})
      .clampScalar(0, s32, s64);

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

  // Extensions: sub-word→s32 handled in instruction selection (AND for zext,
  // SHL+SAR for sext).  s32→s64 narrowed by the framework: splits s64 result
  // into two s32 halves via G_MERGE_VALUES ({src, 0} for zext, etc.).
  getActionDefinitionsBuilder({G_ZEXT, G_SEXT, G_ANYEXT})
      .legalForCartesianProduct({s8, s16, s32}, {s1, s8, s16})
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32));

  // Division/remainder: custom-lower s32 to catch power-of-2 constants
  // (SHR for udiv, AND for urem), fall back to libcalls otherwise.
  // s64 goes straight to libcall (__udivdi3/__umoddi3).
  getActionDefinitionsBuilder({G_UDIV, G_UREM})
      .customFor({s32})
      .libcallFor({s64})
      .clampScalar(0, s32, s64);

  // Signed division/remainder: always libcall (signed power-of-2 lowering
  // needs rounding adjustment — not worth the complexity yet).
  getActionDefinitionsBuilder({G_SDIV, G_SREM})
      .libcallFor({s32, s64})
      .clampScalar(0, s32, s64);

  // Multiplication: custom-lower s32 power-of-2 and power-of-2 ± 1 constants
  // to shifts (+ add/sub), fall back to libcall otherwise.
  // s64 goes straight to libcall (__muldi3).
  getActionDefinitionsBuilder(G_MUL)
      .customFor({s32})
      .libcallFor({s64})
      .clampScalar(0, s32, s64);

  // SEXT_INREG: lowered by framework to SHL+ASHR (our shift constant folding
  // then selects these to SHLi+SARi).
  getActionDefinitionsBuilder(G_SEXT_INREG).lower();

  // Truncation: no-op at the register level (just use the low bits).
  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{s1, s32}, {s8, s32}, {s16, s32}});

  // G_ABS: the optimizer generates this at -O1+ for signed division.
  // Lower to the generic SELECT expansion (icmp + negate + select).
  getActionDefinitionsBuilder(G_ABS).lower();

  // Bit-counting: no hardware instructions, lower to shift/logic sequences.
  getActionDefinitionsBuilder({G_CTTZ, G_CTTZ_ZERO_UNDEF,
                               G_CTLZ, G_CTLZ_ZERO_UNDEF,
                               G_CTPOP})
      .lowerFor({{s32, s32}})
      .clampScalar(0, s32, s32);

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

  // Memory operations: lower to memcpy/memmove/memset libcalls.
  getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE, G_MEMSET}).libcall();

  getLegacyLegalizerInfo().computeTables();
}

bool PenumbraLegalizerInfo::legalizeCustom(
    LegalizerHelper &Helper, MachineInstr &MI,
    LostDebugLocObserver &LocObserver) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;

  // Helper: fall back to a libcall for ops we can't strength-reduce.
  auto Libcall = [&]() {
    return Helper.libcall(MI, LocObserver) == LegalizerHelper::Legalized;
  };

  // Helper: try to read a constant integer from a vreg (looks through COPYs).
  MachineRegisterInfo &MRI = *MIRBuilder.getMRI();
  auto GetConstant = [&](Register Reg) {
    return getIConstantVRegValWithLookThrough(Reg, MRI);
  };

  switch (MI.getOpcode()) {
  default:
    return false;

  case TargetOpcode::G_MUL: {
    Register Dst = MI.getOperand(0).getReg();
    Register Src = MI.getOperand(1).getReg();
    auto MaybeVal = GetConstant(MI.getOperand(2).getReg());
    if (!MaybeVal)
      return Libcall();

    uint64_t C = MaybeVal->Value.getZExtValue();
    LLT Ty = MRI.getType(Dst);

    if (C == 0) {
      MIRBuilder.buildConstant(Dst, 0);
    } else if (C == 1) {
      MIRBuilder.buildCopy(Dst, Src);
    } else if (isPowerOf2_64(C)) {
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C));
      MIRBuilder.buildShl(Dst, Src, ShiftAmt);
    } else if (isPowerOf2_64(C - 1)) {
      // x * (2^n + 1) = (x << n) + x
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C - 1));
      auto Shifted = MIRBuilder.buildShl(Ty, Src, ShiftAmt);
      MIRBuilder.buildAdd(Dst, Shifted, Src);
    } else if (isPowerOf2_64(C + 1)) {
      // x * (2^n - 1) = (x << n) - x
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C + 1));
      auto Shifted = MIRBuilder.buildShl(Ty, Src, ShiftAmt);
      MIRBuilder.buildSub(Dst, Shifted, Src);
    } else {
      return Libcall();
    }

    MI.eraseFromParent();
    return true;
  }

  case TargetOpcode::G_UDIV: {
    Register Dst = MI.getOperand(0).getReg();
    Register Src = MI.getOperand(1).getReg();
    auto MaybeVal = GetConstant(MI.getOperand(2).getReg());
    if (!MaybeVal)
      return Libcall();

    uint64_t C = MaybeVal->Value.getZExtValue();
    if (C == 0 || !isPowerOf2_64(C))
      return Libcall();

    LLT Ty = MRI.getType(Dst);
    if (C == 1) {
      MIRBuilder.buildCopy(Dst, Src);
    } else {
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C));
      MIRBuilder.buildLShr(Dst, Src, ShiftAmt);
    }
    MI.eraseFromParent();
    return true;
  }

  case TargetOpcode::G_UREM: {
    Register Dst = MI.getOperand(0).getReg();
    Register Src = MI.getOperand(1).getReg();
    auto MaybeVal = GetConstant(MI.getOperand(2).getReg());
    if (!MaybeVal)
      return Libcall();

    uint64_t C = MaybeVal->Value.getZExtValue();
    if (C == 0 || !isPowerOf2_64(C))
      return Libcall();

    LLT Ty = MRI.getType(Dst);
    if (C == 1) {
      // x % 1 == 0
      MIRBuilder.buildConstant(Dst, 0);
    } else {
      auto Mask = MIRBuilder.buildConstant(Ty, C - 1);
      MIRBuilder.buildAnd(Dst, Src, Mask);
    }
    MI.eraseFromParent();
    return true;
  }

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

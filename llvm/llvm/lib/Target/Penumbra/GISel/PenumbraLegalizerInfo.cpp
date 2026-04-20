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
      .widenScalarToNextPow2(0, 32)
      .clampScalar(0, s32, s32)
      .scalarize(0);

  // Shifts: clamp both the value (type 0) and shift amount (type 1) to s32.
  // Without clamping type 1, i64 narrowing can produce s64 shift amounts.
  getActionDefinitionsBuilder({G_SHL, G_LSHR, G_ASHR})
      .legalFor({{s32, s32}})
      .widenScalarToNextPow2(0, 32)
      .widenScalarToNextPow2(1, 32)
      .clampScalar(0, s32, s32)
      .clampScalar(1, s32, s32)
      .scalarize(0);

  // Add/sub with overflow and carry: produced by i64 narrowing.
  // All lowered to basic ADD/SUB + ICMP sequences. The hardware has ADC/SBC
  // instructions but using them requires SR flag management that GlobalISel
  // doesn't handle well at -O0. A future peephole pass can fuse
  // ADD+compare+ADC chains into ADD+ADC.
  getActionDefinitionsBuilder({G_UADDO, G_USUBO, G_UADDE, G_USUBE,
                               G_SADDO, G_SSUBO, G_SADDE, G_SSUBE})
      .lowerFor({{s32, s1}})
      .minScalar(0, s32)
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32))
      .lower();

  // Multiply with overflow: lower to wide multiply + overflow check.
  // G_UMULH/G_SMULH (high-half multiply): lower to wide MUL + shift.
  getActionDefinitionsBuilder({G_SMULO, G_UMULO})
      .lowerFor({{s32, s1}})
      .minScalar(0, s32);

  // Saturating add/subtract: no hardware support, lower to the generic
  // add+compare+select expansion (LegalizerHelper picks between the
  // min/max and AddO-based forms depending on legality of the helpers —
  // our G_UADDO/G_USUBO are already lowered, which is what it uses).
  // Sub-word widened to s32; s64 narrowed to s32 pairs.
  getActionDefinitionsBuilder({G_UADDSAT, G_USUBSAT, G_SADDSAT, G_SSUBSAT})
      .lowerFor({s32})
      .widenScalarToNextPow2(0, 32)
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32))
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder({G_UMULH, G_SMULH})
      .lowerFor({s32})
      .minScalar(0, s32);

  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder({G_FRAME_INDEX, G_GLOBAL_VALUE, G_BLOCK_ADDR})
      .legalFor({p0});

  // Penumbra is a strict-alignment target: LDW/STW require 4-byte
  // alignment, LDH/STH require 2-byte alignment, and byte ops are
  // free.  The 4th column below is "minimum alignment in bits" —
  // the predicate isCompatible() checks `Query.AlignInBits >= rule`.
  // Loads/stores that don't meet the alignment fall through to the
  // unaligned `.lowerIf` below, which hands off to LegalizerHelper's
  // lowerLoad/lowerStore for recursive byte-wise splitting.
  //
  // Scope the predicate to hardware-supported memory sizes (8/16/32):
  // wider loads (s64) are narrowed into s32 halves by clampScalar /
  // narrowScalarIf first, and each half reaches the legalizer as an
  // s32 load with its own alignment.  Firing the splitter on an s64
  // too early produces a shift/OR chain whose zero operands never get
  // combined away.
  auto isUnaligned = [=](const LegalityQuery &Q) {
    unsigned MemBits = Q.MMODescrs[0].MemoryTy.getSizeInBits();
    if (MemBits != 8 && MemBits != 16 && MemBits != 32)
      return false;
    return Q.MMODescrs[0].AlignInBits < MemBits;
  };
  getActionDefinitionsBuilder({G_STORE, G_LOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s32, 32},  // LDW/STW  — 4-byte aligned
        {s32, p0, s16, 16},  // LDH/STH  — 2-byte aligned
        {s32, p0, s8,  8},   // LDB/STB  — always aligned
        {p0,  p0, s32, 32},  // pointer load/store — 4-byte aligned
      })
      // Scalarize vectors before the scalar-only widen/clamp rules below,
      // which would otherwise see a vector type and decide the op is
      // unsupported.
      .scalarize(0)
      .widenScalarToNextPow2(0, /* MinSize = */ 8)
      .lowerIfMemSizeNotByteSizePow2()
      .lowerIf(isUnaligned)
      .clampScalar(0, s32, s32);

  // Extending loads: sub-word memory → s32 register is directly supported
  // by LDB/LDBS/LDH/LDHS.  Wider destinations (s64 from optimizer-merged
  // bitfield loads) are narrowed: the helper emits a s32 load + explicit
  // G_ZEXT/G_SEXT to s64, and the extension is then split by our G_ZEXT
  // narrowing rule into two s32 halves (low = loaded value, high = 0 or
  // SAR).  s128 is not yet supported.  Unaligned extending loads are
  // split by the helper (same path as the plain G_LOAD rule above).
  getActionDefinitionsBuilder({G_SEXTLOAD, G_ZEXTLOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s16, 16},
        {s32, p0, s8,   8},
      })
      // Widen sub-s32 destinations first so lowerLoad's recursive split
      // (which preserves the destination type) stays within our s32
      // legal set — otherwise a split G_ZEXTLOAD can surface with e.g.
      // s16 destination that matches no rule.  minScalar (not
      // widenScalarToNextPow2) is required because s16 is already a
      // power of two and the latter would leave it alone.
      .minScalar(0, s32)
      .lowerIf(isUnaligned)
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32));

  getActionDefinitionsBuilder({G_PTR_ADD, G_PTRMASK})
      .legalFor({{p0, s32}});

  // Pointer/integer casts: no-op on Penumbra (pointers = 32-bit integers).
  // G_INTTOPTR: type0=p0 (result), type1=s32 (source).
  // Sub-word sources widened to s32; wider sources narrowed to s32.
  getActionDefinitionsBuilder(G_INTTOPTR)
      .legalFor({{p0, s32}})
      .minScalar(1, s32)
      .maxScalar(1, s32);

  // G_PTRTOINT: type0=s32 (result), type1=p0 (source).
  // Sub-word results widened to s32.  Wider results (e.g. s64 from
  // (uint64_t)(uintptr_t)ptr in libunwind DWARF evaluator) narrowed
  // to s32 — the framework inserts G_ZEXT afterward.
  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalFor({{s32, p0}})
      .minScalar(0, s32)
      .maxScalar(0, s32);

  // Comparisons: G_ICMP produces s1 result, compares s32 operands.
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{s1, s32}, {s1, p0}})
      .widenScalarToNextPow2(1, 32)
      .clampScalar(1, s32, s32);

  // Undefined value: used by the optimizer for uninitialized variables.
  getActionDefinitionsBuilder(G_IMPLICIT_DEF)
      .legalFor({s32, s64, p0})
      .clampScalar(0, s32, s64);

  // PHI nodes at control-flow joins.
  getActionDefinitionsBuilder(G_PHI)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32)
      .scalarize(0);

  // Fences: Penumbra is uniprocessor with no store buffer, so memory fences
  // are pure compiler barriers (no hardware instruction needed).
  getActionDefinitionsBuilder(G_FENCE).alwaysLegal();

  // Traps: __builtin_trap() / __builtin_debugtrap() → BREAK instruction.
  getActionDefinitionsBuilder({G_TRAP, G_DEBUGTRAP}).alwaysLegal();

  // Branches.
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({s1});

  // Indirect branch: G_BRINDIRECT jumps through a pointer (computed goto).
  getActionDefinitionsBuilder(G_BRINDIRECT).legalFor({p0});

  // Jump tables: G_JUMP_TABLE materialises table address, G_BRJT branches.
  getActionDefinitionsBuilder(G_JUMP_TABLE).legalFor({p0});
  getActionDefinitionsBuilder(G_BRJT).alwaysLegal();

  // Select (ternary): result s32/p0, condition s1.
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{s32, s1}, {p0, s1}})
      .clampScalar(0, s32, s32)
      .scalarize(0);

  // Merge/unmerge: used by i64 narrowing (two s32 ↔ one s64) and by the
  // optimizer when building bitfields from individual bits.  Widen small
  // sources to s32 so the framework can handle them.
  for (unsigned Op : {G_MERGE_VALUES, G_UNMERGE_VALUES}) {
    unsigned BigTyIdx = Op == G_MERGE_VALUES ? 0 : 1;
    unsigned LitTyIdx = Op == G_MERGE_VALUES ? 1 : 0;
    getActionDefinitionsBuilder(Op)
        .legalIf(all(typeIs(BigTyIdx, s64), typeIs(LitTyIdx, s32)))
        .widenScalarToNextPow2(LitTyIdx, 32)
        .widenScalarToNextPow2(BigTyIdx, 32)
        .clampScalar(LitTyIdx, s32, s32)
        .clampScalar(BigTyIdx, s32, s64);
  }

  // Extensions: sub-word→s32 handled in instruction selection (AND for zext,
  // SHL+SAR for sext).  s32→s64 narrowed by the framework: splits s64 result
  // into two s32 halves via G_MERGE_VALUES ({src, 0} for zext, etc.).
  getActionDefinitionsBuilder({G_ZEXT, G_SEXT, G_ANYEXT})
      .legalForCartesianProduct({s8, s16, s32}, {s1, s8, s16})
      .widenScalarToNextPow2(0, 32)
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32));

  // Division/remainder: custom-lower s32 to catch power-of-2 constants
  // (SHR for udiv, AND for urem), fall back to libcalls otherwise.
  // s64 goes straight to libcall (__udivdi3/__umoddi3).
  getActionDefinitionsBuilder({G_UDIV, G_UREM})
      .customFor({s32})
      .libcallFor({s64})
      .clampScalar(0, s32, s64)
      .scalarize(0);

  // Signed division/remainder: always libcall (signed power-of-2 lowering
  // needs rounding adjustment — not worth the complexity yet).
  getActionDefinitionsBuilder({G_SDIV, G_SREM})
      .libcallFor({s32, s64})
      .clampScalar(0, s32, s64)
      .scalarize(0);

  // Multiplication: custom-lower s32 power-of-2 and power-of-2 ± 1 constants
  // to shifts (+ add/sub), fall back to libcall otherwise.
  // s64 goes straight to libcall (__muldi3).
  // Non-power-of-2 widths (e.g. i33 from SCEV's closed-form sum-of-
  // arithmetic-progression rewrite at -O2) widen to next pow2 first,
  // so an s33 MUL lands on s64 and libcalls like any other i64 mul.
  getActionDefinitionsBuilder(G_MUL)
      .customFor({s32})
      .libcallFor({s64})
      .widenScalarToNextPow2(0, 32)
      .clampScalar(0, s32, s64)
      .scalarize(0);

  // SEXT_INREG: lowered by framework to SHL+ASHR (our shift constant folding
  // then selects these to SHLi+SARi).
  getActionDefinitionsBuilder(G_SEXT_INREG).lower();

  // Truncation: no-op at the register level (just use the low bits).
  // Accept any source width (odd widths like s33 from G_SADDO lowering).
  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{s1, s32}, {s8, s32}, {s16, s32}})
      .alwaysLegal();

  // Funnel shifts: used by compiler-rt __udivsi3, by -O2 rotate-idiom
  // recognition on any integer width, and directly by llvm.fshl/fshr.
  // LegalizerHelper::lowerFunnelShiftAsShifts emits shl/lshr/or at the
  // *original* width; sub-word widths (s8/s16) get legalized afterward
  // via our G_SHL/G_LSHR widen rules (ZEXT source, TRUNC result — safe
  // because lower leaves no high-bit dependencies that would leak
  // through ANYEXT/ZEXT).
  getActionDefinitionsBuilder({G_FSHL, G_FSHR}).lower();

  // Byte swap: used by SHA1, networking, etc. Lower to shift/mask/OR.
  // Bitreverse: similar, lower to shift/mask sequence.
  // Sub-word (s8/s16) widened to s32 first, then lowered.
  getActionDefinitionsBuilder({G_BSWAP, G_BITREVERSE})
      .lowerFor({s32, s64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64);

  // Min/max: lower to icmp + select for any scalar width.
  // s64 lowers to icmp+select at s64 level, then the framework narrows
  // those via existing s64→s32 rules (G_ICMP clampScalar, G_SELECT).
  getActionDefinitionsBuilder({G_SMIN, G_SMAX, G_UMIN, G_UMAX})
      .lower();

  // G_ABS: the optimizer generates this at -O1+ for signed division.
  // Lower to the generic SELECT expansion (icmp + negate + select).
  getActionDefinitionsBuilder(G_ABS).lower();

  // Bit-counting: no hardware instructions, lower to shift/logic sequences.
  getActionDefinitionsBuilder({G_CTTZ, G_CTTZ_ZERO_UNDEF,
                               G_CTLZ, G_CTLZ_ZERO_UNDEF,
                               G_CTPOP})
      .lowerFor({{s32, s32}})
      .narrowScalarIf(typeIs(1, s64), changeTo(1, s32))
      .clampScalar(0, s32, s32)
      .clampScalar(1, s32, s32);

  // G_FREEZE: converts potentially-poison values to well-defined ones.
  // At -O1+ the optimizer inserts these around division and other ops.
  // No-op on Penumbra — just pass the value through.
  getActionDefinitionsBuilder(G_FREEZE)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  // Varargs: G_VASTART stores the save area address into the va_list.
  // G_VAARG is custom-lowered to a pointer bump + load WITHOUT
  // alignment rounding — our CC passes i64 halves in consecutive
  // 4-byte slots without alignment gaps, so va_arg must not skip
  // slots to reach 8-byte alignment.
  getActionDefinitionsBuilder(G_VASTART).customFor({p0});
  getActionDefinitionsBuilder(G_VAARG)
      .clampScalar(0, s32, s64)
      .customForCartesianProduct({s32, s64, p0}, {p0});

  // Stack save/restore/dynamic alloc: used by alloca.  SP is R14.
  // G_DYN_STACKALLOC is lowered by the framework to SP subtract + alignment.
  getActionDefinitionsBuilder(G_DYN_STACKALLOC).lower();
  getActionDefinitionsBuilder(G_STACKSAVE).legalFor({p0});
  getActionDefinitionsBuilder(G_STACKRESTORE).legalFor({p0});

  // FP rounding mode: no FPU, always "round to nearest" (1).
  // Custom-lower to a constant.
  getActionDefinitionsBuilder(G_GET_ROUNDING).customFor({s32});

  // Prefetch: no cache hints on Penumbra, just discard.
  getActionDefinitionsBuilder(G_PREFETCH).custom();

  // Floating-point operations: Penumbra has no FPU, everything goes to
  // libcalls (__addsf3, __fixunsdfsi, __floatsidf, etc.).
  // G_FNEG/G_FABS/G_FCOPYSIGN/G_IS_FPCLASS: pure bit manipulation
  // (flip/clear/copy sign bit, exponent/mantissa inspection).
  // No libcall exists — lower to integer ops on the bit pattern.
  getActionDefinitionsBuilder({G_FNEG, G_FABS, G_FCOPYSIGN})
      .lowerFor({s32, s64});
  getActionDefinitionsBuilder(G_IS_FPCLASS)
      .lowerFor({{s1, s32}, {s1, s64}});

  getActionDefinitionsBuilder({G_FADD, G_FSUB, G_FMUL, G_FDIV, G_FREM,
                               G_FSQRT,
                               G_FMINNUM, G_FMAXNUM,
                               G_FMINIMUM, G_FMAXIMUM,
                               G_FMA, G_FMAD,
                               G_FCEIL, G_FFLOOR, G_FRINT, G_FNEARBYINT,
                               G_INTRINSIC_ROUND, G_INTRINSIC_ROUNDEVEN,
                               G_INTRINSIC_TRUNC,
                               G_FLOG, G_FLOG2, G_FLOG10,
                               G_FEXP, G_FEXP2, G_FPOW,
                               G_FSIN, G_FCOS, G_FTAN,
                               G_FASIN, G_FACOS, G_FATAN, G_FATAN2,
                               G_FSINH, G_FCOSH, G_FTANH,
                               G_FLDEXP, G_FMODF})
      .libcallFor({s32, s64})
      .scalarize(0);

  // G_FSINCOS: returns two FP values (sin + cos).
  getActionDefinitionsBuilder(G_FSINCOS)
      .libcallFor({{s32, s32}, {s64, s64}});

  // G_FFREXP: returns mantissa + exponent; G_FCANONICALIZE: no-op lower.
  getActionDefinitionsBuilder(G_FFREXP)
      .libcallFor({{s32, s32}, {s64, s32}});
  getActionDefinitionsBuilder(G_FCANONICALIZE)
      .lowerFor({s32, s64});

  // FP→int: widen sub-word int results (s8/s16) to s32 before libcall.
  // int→FP: widen sub-word int sources (s8/s16) to s32 before libcall.
  getActionDefinitionsBuilder({G_FPTOUI, G_FPTOSI})
      .libcallFor({{s32, s32}, {s32, s64}, {s64, s32}, {s64, s64}})
      .minScalar(0, s32);

  getActionDefinitionsBuilder({G_UITOFP, G_SITOFP})
      .libcallFor({{s32, s32}, {s64, s32}, {s32, s64}, {s64, s64}})
      .minScalar(1, s32);

  getActionDefinitionsBuilder(G_FPEXT)
      .libcallFor({{s64, s32}});

  getActionDefinitionsBuilder(G_FPTRUNC)
      .libcallFor({{s32, s64}});

  getActionDefinitionsBuilder(G_FCMP)
      .libcallFor({{s1, s32}, {s1, s64}});

  getActionDefinitionsBuilder(G_FCONSTANT)
      .customFor({s32, s64});

  // Atomic operations: all handled via __atomic_* libcalls.
  // Clang emits libcalls directly (MaxAtomicInlineWidth = 0), so no
  // G_ATOMICRMW / G_ATOMIC_CMPXCHG should reach the legalizer.
  // If they somehow do (e.g. from IR), fall back to libcall.
  getActionDefinitionsBuilder({G_ATOMICRMW_XCHG, G_ATOMICRMW_ADD,
                               G_ATOMICRMW_SUB, G_ATOMICRMW_AND,
                               G_ATOMICRMW_NAND, G_ATOMICRMW_OR,
                               G_ATOMICRMW_XOR, G_ATOMICRMW_MAX,
                               G_ATOMICRMW_MIN, G_ATOMICRMW_UMAX,
                               G_ATOMICRMW_UMIN})
      .libcallFor({{s32, p0}});

  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG)
      .libcallFor({{s32, p0}});

  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG_WITH_SUCCESS)
      .lower();

  // Memory operations: lower to memcpy/memmove/memset libcalls.
  getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE, G_MEMSET}).libcall();

  // Penumbra has no vector unit.  The front end accepts GCC
  // vector_size/OpenCL vector extensions and the IR optimizer can
  // auto-vectorize, so vector ops do reach the legalizer — we
  // unroll them into per-element scalar ops that the rules above
  // then handle normally.  G_BITCAST between vector and scalar (e.g.
  // (<2 x s32>) <-> s64 from unions) lowers to G_UNMERGE/G_MERGE,
  // which our existing narrow rules already cover.
  getActionDefinitionsBuilder(G_BITCAST)
      .legalFor({{s32, s32}, {s32, p0}, {p0, s32}, {s64, s64}})
      .lower();

  getActionDefinitionsBuilder({G_EXTRACT_VECTOR_ELT, G_INSERT_VECTOR_ELT})
      .lower();

  getActionDefinitionsBuilder(G_BUILD_VECTOR).lower();
  getActionDefinitionsBuilder(G_SHUFFLE_VECTOR).lower();
  getActionDefinitionsBuilder(G_CONCAT_VECTORS).lower();

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
    unsigned BitWidth = Ty.getSizeInBits();

    if (C == 0) {
      MIRBuilder.buildConstant(Dst, 0);
    } else if (C == 1) {
      MIRBuilder.buildCopy(Dst, Src);
    } else if (C == maskTrailingOnes<uint64_t>(BitWidth)) {
      // x * -1 = 0 - x (negate)
      auto Zero = MIRBuilder.buildConstant(Ty, 0);
      MIRBuilder.buildSub(Dst, Zero, Src);
    } else if (isPowerOf2_64(C) && Log2_64(C) < BitWidth) {
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C));
      MIRBuilder.buildShl(Dst, Src, ShiftAmt);
    } else if (isPowerOf2_64(C - 1) && Log2_64(C - 1) < BitWidth) {
      // x * (2^n + 1) = (x << n) + x
      auto ShiftAmt = MIRBuilder.buildConstant(Ty, Log2_64(C - 1));
      auto Shifted = MIRBuilder.buildShl(Ty, Src, ShiftAmt);
      MIRBuilder.buildAdd(Dst, Shifted, Src);
    } else if (isPowerOf2_64(C + 1) && Log2_64(C + 1) < BitWidth) {
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

  case TargetOpcode::G_PREFETCH:
    // No cache hints on Penumbra — just discard the prefetch.
    MI.eraseFromParent();
    return true;

  case TargetOpcode::G_GET_ROUNDING: {
    // No FPU — always round to nearest (1).
    Register Dst = MI.getOperand(0).getReg();
    MIRBuilder.buildConstant(Dst, 1);
    MI.eraseFromParent();
    return true;
  }

  case TargetOpcode::G_VAARG: {
    // Custom va_arg: load value from va_list pointer, advance by size.
    // No alignment rounding — our ABI uses 4-byte stack slots for all
    // types including i64 (consecutive halves, no gap).
    Register Dst = MI.getOperand(0).getReg();
    Register ListPtr = MI.getOperand(1).getReg();
    LLT DstTy = MRI.getType(Dst);
    LLT PtrTy = LLT::pointer(0, 32);
    unsigned Size = DstTy.getSizeInBytes();

    // Load current va_list pointer value
    auto CurPtr = MIRBuilder.buildLoad(PtrTy, ListPtr,
        *MIRBuilder.getMF().getMachineMemOperand(
            MachinePointerInfo(), MachineMemOperand::MOLoad, PtrTy, Align(4)));

    // Load the argument value from that address
    MIRBuilder.buildLoad(Dst, CurPtr,
        *MIRBuilder.getMF().getMachineMemOperand(
            MachinePointerInfo(), MachineMemOperand::MOLoad, DstTy, Align(4)));

    // Advance pointer by argument size
    auto SizeConst = MIRBuilder.buildConstant(LLT::scalar(32), Size);
    auto NextPtr = MIRBuilder.buildPtrAdd(PtrTy, CurPtr, SizeConst);

    // Store updated pointer back to va_list
    MIRBuilder.buildStore(NextPtr, ListPtr,
        *MIRBuilder.getMF().getMachineMemOperand(
            MachinePointerInfo(), MachineMemOperand::MOStore, PtrTy, Align(4)));

    MI.eraseFromParent();
    return true;
  }

  case TargetOpcode::G_FCONSTANT: {
    // Materialize FP constant as an integer bit pattern.
    Register Dst = MI.getOperand(0).getReg();
    LLT Ty = MRI.getType(Dst);
    const ConstantFP *CFP = MI.getOperand(1).getFPImm();
    APInt IntVal = CFP->getValueAPF().bitcastToAPInt();

    if (Ty.getSizeInBits() == 32) {
      MIRBuilder.buildConstant(Dst, IntVal.getZExtValue());
    } else {
      // s64: split into two s32 halves via merge
      auto Lo = MIRBuilder.buildConstant(LLT::scalar(32),
                                         IntVal.getLoBits(32).getZExtValue());
      auto Hi = MIRBuilder.buildConstant(LLT::scalar(32),
                                         IntVal.getHiBits(32).getZExtValue());
      MIRBuilder.buildMergeLikeInstr(Dst, {Lo, Hi});
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

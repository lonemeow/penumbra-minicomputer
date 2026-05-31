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

  // Multiply with overflow.  Asymmetric at s64 — matches SelectionDAG's
  // ExpandIntRes_XMULO (which inline-expands UMULO i64 and libcalls SMULO i64
  // to __mulodi4 when available).
  //
  // G_UMULO at s32/s64: .lower() expands to G_MUL + G_UMULH + ICMP_NE at the
  // original width.  At s32 the G_UMULH lowers via widening to s64 (so the
  // wide G_MUL libcalls __muldi3 once).  At s64 the G_UMULH narrows to s32
  // partial products via the G_UMULH rule below.  The trailing .lower() is a
  // safety net for any width that didn't match lowerFor after widening.
  //
  // G_SMULO at s32: same .lower() expansion via G_SMULH at s32.
  // G_SMULO at s64: deliberately unhandled — will hard-error if it ever
  // appears.  No clean path exists today (narrowScalarMul doesn't handle
  // G_SMULH, and LegalizerHelper::libcall has no G_SMULO case).  When it
  // does come up, the fix is a custom legalization that emits a __mulodi4
  // libcall — mulodi4.c is already linked into NetBSD's libc and our
  // bare-metal compiler-rt builtins, so the symbol is always available.
  getActionDefinitionsBuilder(G_UMULO)
      .lowerFor({{s32, s1}, {s64, s1}})
      .minScalar(0, s32)
      .lower();

  getActionDefinitionsBuilder(G_SMULO)
      .lowerFor({{s32, s1}})
      .minScalar(0, s32);

  // Saturating add/subtract: no hardware support, lower to the generic
  // add+compare+select expansion (LegalizerHelper picks between the
  // min/max and AddO-based forms depending on legality of the helpers —
  // our G_UADDO/G_USUBO are already lowered, which is what it uses).
  // Sub-word widened to s32.  s64 is lowered at native width into
  // G_UMIN/G_SUB (or G_USUBO+G_SELECT); the legalizer's iterative
  // pass then narrows those to s32 pairs (G_SUB via the G_USUBE
  // carry chain, G_UMIN via lower → ICMP+SELECT → narrow).
  getActionDefinitionsBuilder({G_UADDSAT, G_USUBSAT, G_SADDSAT, G_SSUBSAT})
      .lowerFor({s32, s64})
      .widenScalarToNextPow2(0, 32)
      .clampScalar(0, s32, s64);

  // High-half multiply: s32 is legal, selected to the MULU_P/MUL_P pair form
  // (the unit produces both halves; the selector keeps the high one).  This is
  // what lets a 32x32->64 widening multiply use the hardware instead of a
  // __muldi3 libcall.
  // - G_UMULH s64: narrowScalarMul splits into s32 partial products, now legal
  //   hardware multiplies.  Reached by the G_UMULO s64 .lower() path.
  // - G_SMULH s64: not reachable today (G_SMULO s64 is unhandled).
  getActionDefinitionsBuilder(G_UMULH)
      .legalFor({s32})
      .minScalar(0, s32)
      .narrowScalarIf(typeIs(0, s64), changeTo(0, s32));

  getActionDefinitionsBuilder(G_SMULH)
      .legalFor({s32})
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
  // bitfield loads, s33 from `__builtin_add_overflow` mixed-sign at -O0
  // before InstCombine has a chance to fold) are clamped to s32 — the
  // framework then re-extends to the original type via G_ZEXT/G_SEXT,
  // which our G_ZEXT narrowing rule splits into s32 halves at s64 (low
  // = loaded value, high = 0 or SAR replication).  Anything left after
  // clamping that doesn't match the legalFor set (e.g. dst==mem after
  // clamping a wider dst down to s32 over an s32 load) is lowered to a
  // plain G_LOAD + extension.  s128 is not yet supported.  Unaligned
  // extending loads split via the helper, same as G_LOAD.
  getActionDefinitionsBuilder({G_SEXTLOAD, G_ZEXTLOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s16, 16},
        {s32, p0, s8,   8},
      })
      .lowerIf(isUnaligned)
      .widenScalarToNextPow2(0, /* MinSize = */ 32)
      .clampScalar(0, s32, s32)
      .lower();

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

  // Unsigned division/remainder: custom-lower s32 to catch power-of-2 constants
  // (SHR for udiv, AND for urem — cheaper than a 34-cycle divide); the custom
  // handler leaves the variable case in place as a legal s32 op, which the
  // selector maps onto the hardware DIVU/DIVU_P peer unit.
  // s64 goes straight to libcall (__udivdi3/__umoddi3).
  getActionDefinitionsBuilder({G_UDIV, G_UREM})
      .customFor({s32})
      .libcallFor({s64})
      .clampScalar(0, s32, s64)
      .scalarize(0);

  // Signed division/remainder: s32 is legal, selected to the hardware DIV/DIV_P
  // peer unit.  Unlike the unsigned path we add no custom power-of-2 lowering:
  // the -O1+ combiner already strength-reduces signed pow2 divides to a shift
  // sequence, and adding it for -O0 isn't worth the rounding-bias complexity.
  // s64 libcalls.
  getActionDefinitionsBuilder({G_SDIV, G_SREM})
      .legalFor({s32})
      .libcallFor({s64})
      .clampScalar(0, s32, s64)
      .scalarize(0);

  // Fused divide+remainder: the divmul unit produces quotient and remainder in
  // one operation, so s32 G_S/UDIVREM is legal and selects to a single
  // DIV_P/DIVU_P.  The -O1+ combiner fuses adjacent div/rem on shared operands
  // into this form, making `a/b; a%b` cost one divide.  s64 has no pair form, so
  // .lower() splits it into separate G_S/UDIV + G_S/UREM, which libcall.
  getActionDefinitionsBuilder({G_SDIVREM, G_UDIVREM})
      .legalFor({s32})
      .clampScalar(0, s32, s64)
      .lower();

  // Multiplication, custom-lowered at both widths:
  // - s32: strength-reduce power-of-2 and power-of-2 ± 1 constants to shifts
  //   (+ add/sub); the variable case stays a legal s32 op selected to MUL.
  // - s64: a 32x32->64 widening multiply (both operands extended from s32) is
  //   a single hardware multiply, emitted as a low/high pair (G_MUL +
  //   G_S/UMULH); any other i64 multiply narrows to the generic schoolbook.
  // Non-power-of-2 widths (e.g. i33 from SCEV's closed-form sum-of-arithmetic-
  // progression rewrite at -O2) widen to next pow2 first, landing on s64.
  getActionDefinitionsBuilder(G_MUL)
      .customFor({s32, s64})
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

  // Rotates: synthesized by the pre-legalizer combiner from `(x << n) |
  // (x >> (W-n))` idioms (funnel_shift_combines).  No hardware rotate, so
  // lower back to shift/or — same shape we'd have selected from the
  // original IR if the combiner hadn't recognised the pattern.
  getActionDefinitionsBuilder({G_ROTL, G_ROTR}).lower();

  // Byte swap: used by SHA1, networking, etc. Lower to shift/mask/OR.
  // Bitreverse: similar, lower to shift/mask sequence.
  // Sub-word (s8/s16) widened to s32 first.  s64 narrowed to two s32.
  // Then the s32 lowering runs.  We can't just `.lowerFor({s32, s64})`
  // because LegalizerHelper::lowerBswap has a signed-int-shift UB in
  // its per-byte mask computation that corrupts the mask for byte 3
  // of an s64 (APInt(64, 0xFF << 24) sign-extends into the high
  // half).  Other 32-bit targets (e.g. RISC-V) avoid this path by
  // narrowing first, so the bug never trips.  Match that convention.
  getActionDefinitionsBuilder({G_BSWAP, G_BITREVERSE})
      .lowerFor({s32})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s32);

  // Min/max: lower to icmp + select for any scalar width.
  // s64 lowers to icmp+select at s64 level, then the framework narrows
  // those via existing s64→s32 rules (G_ICMP clampScalar, G_SELECT).
  getActionDefinitionsBuilder({G_SMIN, G_SMAX, G_UMIN, G_UMAX})
      .lower();

  // Three-way compare: clang at -O2 recognises the (a>b)-(a<b) qsort
  // comparator idiom and emits @llvm.scmp / @llvm.ucmp, which the
  // IRTranslator turns into G_SCMP / G_UCMP.  The result is a small
  // integer in {-1, 0, +1}; the source operands can be any scalar
  // width (i32 / i64 in practice).  Lower via the generic helper
  // LegalizerHelper::lowerThreewayCompare(), which emits two G_ICMPs
  // plus a subtract — both of which our existing s32/s64 rules
  // already handle.
  getActionDefinitionsBuilder({G_SCMP, G_UCMP}).lower();

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

  // lround/llround/lrint/llrint: rounding FP→int per libm semantics
  // (round-to-nearest-away from 0 for lround/llround, round-to-current-mode
  // for lrint/llrint).  Distinct from G_FPTOSI because libm fixes the
  // rounding mode in the call.  No FPU — straight to libcall.
  //  - G_LROUND/G_INTRINSIC_LRINT: result `long` (s32 on Penumbra).
  //  - G_LLROUND/G_INTRINSIC_LLRINT: result `long long` (s64).
  // Source is f32 (s32) or f64 (s64).
  getActionDefinitionsBuilder({G_LROUND, G_INTRINSIC_LRINT})
      .libcallFor({{s32, s32}, {s32, s64}});

  getActionDefinitionsBuilder({G_LLROUND, G_INTRINSIC_LLRINT})
      .libcallFor({{s64, s32}, {s64, s64}});

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
  (void)LocObserver;

  // Helper: leave the variable / non-strength-reducible case in place as a legal
  // s32 op for the selector to map onto the hardware divmul unit.  Returning
  // Legalized without changing MI is safe — the Legalizer only revisits
  // instructions an observer reports as changed.
  auto LeaveForHardware = [&]() { return true; };

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
    LLT Ty = MRI.getType(Dst);

    // s64: turn a 32x32->64 widening multiply into one hardware multiply.  When
    // both operands are the same kind of extension from s32, the full 64-bit
    // product is {G_MUL(a,b) : G_S/UMULH(a,b)} on the s32 sources (the selector
    // maps the high half to MUL_P/MULU_P).  Other i64 multiplies fall through to
    // the generic partial-product schoolbook.
    if (Ty.getSizeInBits() == 64) {
      const LLT S32 = LLT::scalar(32);
      auto ExtSrc = [&](Register R, unsigned ExtOpc) -> Register {
        MachineInstr *Def = getDefIgnoringCopies(R, MRI);
        if (Def && Def->getOpcode() == ExtOpc &&
            MRI.getType(Def->getOperand(1).getReg()) == S32)
          return Def->getOperand(1).getReg();
        return Register();
      };
      Register LHS = MI.getOperand(1).getReg();
      Register RHS = MI.getOperand(2).getReg();
      Register SA = ExtSrc(LHS, TargetOpcode::G_SEXT);
      Register SB = ExtSrc(RHS, TargetOpcode::G_SEXT);
      Register ZA = ExtSrc(LHS, TargetOpcode::G_ZEXT);
      Register ZB = ExtSrc(RHS, TargetOpcode::G_ZEXT);
      bool Signed = SA.isValid() && SB.isValid();
      if (Signed || (ZA.isValid() && ZB.isValid())) {
        Register A = Signed ? SA : ZA;
        Register B = Signed ? SB : ZB;
        auto Lo = MIRBuilder.buildMul(S32, A, B);
        auto Hi = Signed ? MIRBuilder.buildSMulH(S32, A, B)
                         : MIRBuilder.buildUMulH(S32, A, B);
        MIRBuilder.buildMergeLikeInstr(Dst, {Lo.getReg(0), Hi.getReg(0)});
        MI.eraseFromParent();
        return true;
      }
      return Helper.narrowScalar(MI, 0, S32) == LegalizerHelper::Legalized;
    }

    Register Src = MI.getOperand(1).getReg();
    auto MaybeVal = GetConstant(MI.getOperand(2).getReg());
    if (!MaybeVal)
      return LeaveForHardware();

    uint64_t C = MaybeVal->Value.getZExtValue();
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
      return LeaveForHardware();
    }

    MI.eraseFromParent();
    return true;
  }

  case TargetOpcode::G_UDIV: {
    Register Dst = MI.getOperand(0).getReg();
    Register Src = MI.getOperand(1).getReg();
    auto MaybeVal = GetConstant(MI.getOperand(2).getReg());
    if (!MaybeVal)
      return LeaveForHardware();

    uint64_t C = MaybeVal->Value.getZExtValue();
    if (C == 0 || !isPowerOf2_64(C))
      return LeaveForHardware();

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
      return LeaveForHardware();

    uint64_t C = MaybeVal->Value.getZExtValue();
    if (C == 0 || !isPowerOf2_64(C))
      return LeaveForHardware();

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

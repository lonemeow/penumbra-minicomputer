# LLVM GlobalISel cannot legalize wide-memory extending loads — riscv32 ICEs on portable C

## TL;DR

GlobalISel has no working path for an *aligned* extending load whose
memory type is register-width or wider — the shape the extload
combine creates when a load feeds a wider-than-legal `zext`, e.g.
`G_ZEXTLOAD s128 ← (load s64)` on a 32-bit target.  Both generic
strategies a target ruleset can request bail with `UnableToLegalize`:

- **`LegalizerHelper::narrowScalar`** (`G_ZEXTLOAD`/`G_SEXTLOAD`
  case): when `MemSize > NarrowSize`, returns `UnableToLegalize` at a
  literal `// FIXME: Need to split the load.`
- **`LegalizerHelper::lowerLoad`**: for a power-of-2 memory size it
  *assumes* lowering was requested because the access is unaligned
  ("Assume we're being asked to decompose an unaligned load") and
  returns `UnableToLegalize` when `TLI.allowsMemoryAccess()` says the
  access is fine — even though the function's own header comment
  promises "Lower to a memory-width G_LOAD and a
  G_SEXT/G_ZEXT/G_ANYEXT".

In-tree witness: **riscv32 with `-global-isel` ICEs on portable C**
at -O2 (via the `narrowScalar` hole — its extload rules end in
`clampScalar(0, sXLen, sXLen)`):

```
LLVM ERROR: unable to legalize instruction:
  %5:_(s128) = G_ZEXTLOAD %0:_(p0) :: (load (s64) from %ir.p)  (in function: umulh64_load)
```

Verified against LLVM 22.1.2 (this tree's base) with a RISCV-only
`llc` build.

## How the shape arises from portable C

Two independent front-end paths produce wider-than-legal integers,
so a 32-bit GlobalISel target cannot rule them out:

1. **`AggressiveInstCombine::foldMulHigh`** (runs at -O2/-O3)
   recognizes the schoolbook high-half multiply — four 32×32 partial
   products summed with carries, the textbook `umulh` every hash
   function's portable fallback spells out — and rewrites it to
   `zext iN → i2N; mul; lshr N; trunc`.  The fold is **completely
   ungated**: no TTI hook, no DataLayout legal-integer check, any
   even bit width.  On a 32-bit target with `i64` inputs it
   manufactures `i128`.
2. **C23 `_BitInt(128)`** arithmetic reaches the backend as `i128`
   directly, at any optimization level.

The load is then folded in by the IRTranslator/extload combine
(`combines_for_extload`), which is legality-blind before the
legalizer by design: `zext (load i64) to i128` becomes
`G_ZEXTLOAD s128 ← s64`.  At that point the target needs one of the
two generic strategies above, and both are holes.

## Reproducer

```llvm
define i64 @umulh64_load(ptr %p, i64 %b) {
  %a = load i64, ptr %p, align 8
  %xe = zext i64 %a to i128
  %ye = zext i64 %b to i128
  %m = mul nuw i128 %xe, %ye
  %hi = lshr i128 %m, 64
  %t = trunc i128 %hi to i64
  ret i64 %t
}
```

```sh
llc -mtriple=riscv32 -mattr=+m -global-isel -global-isel-abort=1 -O2 repro.ll
```

The same IR comes out of `clang -O2` for this C (which is why the
report is not contrived — `foldMulHigh` produces it from code in the
wild):

```c
typedef unsigned long long u64;
#define loWord(a) ((a) & 0xffffffffU)
#define hiWord(a) ((a) >> 32)
u64 mulhi64_load(const u64 *p, u64 b) {
  u64 a = *p;
  const u64 plolo = loWord(a) * loWord(b);
  const u64 plohi = loWord(a) * hiWord(b);
  const u64 philo = hiWord(a) * loWord(b);
  const u64 phihi = hiWord(a) * hiWord(b);
  const u64 r1 = hiWord(plolo) + loWord(plohi) + loWord(philo);
  return hiWord(plohi) + hiWord(philo) + hiWord(r1) + phihi;
}
```

Notably the **register-only variants all legalize fine on riscv32**
(`mul`/`mulhu` schoolbook expansion via `narrowScalarMul`): the
zext/sext/mul/lshr/trunc-at-s128 machinery is in place.  The
extending load is the *only* missing piece, which makes for a
minimal, well-localized report.

## Why each target does or doesn't hit it

| Target   | Extload rule tail (paraphrased)            | `G_ZEXTLOAD s128 ← s64` outcome |
|----------|---------------------------------------------|----------------------------------|
| riscv32  | `clampScalar(0, sXLen, sXLen)` then `lower()` | clamp matches first → `narrowScalar` to s32 → `MemSize (64) > NarrowSize (32)` → **FIXME, ICE** |
| AArch64  | `clampScalar(0, s32, s64)` then `lower()`    | clamp to s64 → `narrowScalar` with `MemSize == NarrowSize` (the one working case: plain load + ext) → escapes |
| Penumbra (this tree) | `lowerIf(mem > 32)` → `lower()`   | hit the `lowerLoad` hole; fixed locally (see below) |

AArch64 escapes by *coincidence of widths* — its clamp ceiling
equals the memory size, landing on the only `narrowScalar` case
that works.  Its own builder comment ("Lower anything left over
into G_*EXT and G_LOAD") documents the very `lowerLoad` behavior
that does not exist.

Note that **no ruleset change can fix riscv32**: the only two
actions a target can request for this shape are NarrowScalar and
Lower, and both handlers bail.  Re-routing the wide extload to
`lower()` (as this tree's target does) merely trades the
`narrowScalar` FIXME for the `lowerLoad` assume-unaligned bail.
That is what makes this a generic LegalizerHelper bug rather than a
target-ruleset bug — and riscv32's `clampScalar` idiom is not wrong,
it is a legitimate request the helper fails to honor.

## Proposed fix

One canonical transform, implemented once: **emit a memory-width
`G_LOAD` plus a separate `G_SEXT`/`G_ZEXT`/`G_ANYEXT`** and let
iterative legalization narrow each piece (the wide `G_LOAD` splits
via `reduceLoadStoreWidth`, the extension via `narrowScalarExt`).

The canonical home is `lowerLoad`: un-fusing a compound op into its
constituents is what the Lower action *means*, the function's
header comment already claims exactly this decomposition, and the
`narrowScalar` extload case is a half-implementation of it anyway —
for the sizes it does handle it loads into a temp and extends **to
the original destination type**, i.e. it never actually narrows the
destination.  So the two-hole shape of the bug should not become a
two-implementation shape in the fix:

1. Add the missing aligned case to `lowerLoad` (patch below).
2. Replace the `MemSize > NarrowSize` FIXME in `narrowScalar`'s
   `G_ZEXTLOAD`/`G_SEXTLOAD` case with a delegation:
   `return lowerLoad(LoadMI);` — correct for the unaligned wide
   case too, since `lowerLoad`'s existing split path covers it.

This tree already carries the `lowerLoad` half (in
`llvm/lib/CodeGen/GlobalISel/LegalizerHelper.cpp`, the power-of-2
branch of the split logic):

```cpp
    auto &Ctx = MF.getFunction().getContext();
    if (TLI.allowsMemoryAccess(Ctx, MIRBuilder.getDataLayout(), MemTy, MMO)) {
      if (DstTy.getSizeInBits() > MemSizeInBits) {
        auto NewLoad = MIRBuilder.buildLoad(MemTy, PtrReg, MMO);
        unsigned ExtOpc = isa<GSExtLoad>(LoadMI)   ? TargetOpcode::G_SEXT
                          : isa<GZExtLoad>(LoadMI) ? TargetOpcode::G_ZEXT
                                                   : TargetOpcode::G_ANYEXT;
        MIRBuilder.buildInstr(ExtOpc, {DstReg}, {NewLoad});
        LoadMI.eraseFromParent();
        return Legalized;
      }
      return UnableToLegalize;
    }
```

An upstream patch should carry both, plus pre-legalizer MIR tests
(`-run-pass=legalizer`) for the extload at {s128, s64-mem} on
riscv32 and an end-to-end `llc` test from the IR above.  A
defensible companion (or alternative) is gating `foldMulHigh` on
something like `DataLayout::isLegalInteger(2 * BitWidth)` — but the
legalizer fix is the more general cure, since `_BitInt` reaches the
same gap without any idiom recognition, and the fold is a genuine
codegen win at widths the target can narrow (on this tree's 32-bit
target, a hand-written 32×32 `mulhi32` collapses to a single
hardware `MULU` thanks to the fold).

## Status in this tree

- The Penumbra ruleset routes wide-memory extloads to `lower()`
  (`PenumbraLegalizerInfo.cpp`, extload builder) and the local
  `lowerLoad` fix above makes that work; extension narrowing was
  widened from `typeIs(s64)` to `scalarWiderThan(32)` in the same
  change.  Regression test: `test/CodeGen/Penumbra/i128-mulhi.ll`.
- The `narrowScalar` FIXME is **not** patched locally — Penumbra
  never routes extloads to it.  It is the half riscv32 needs, left
  for the upstream submission.
- `build/llvm-riscv/` (RISCV-only `llc` configured from this tree)
  was used to verify the riscv32 ICE; disposable.

# LLVM GlobalISel `LegalizerHelper::lowerBswap` miscompiles `G_BSWAP` on widths > 32 bits

## TL;DR

In `llvm/lib/CodeGen/GlobalISel/LegalizerHelper.cpp`, `lowerBswap`
constructs per-byte masks with

```cpp
APInt APMask(SizeInBytes * 8, 0xFF << (i * 8));
```

The inner expression `0xFF << (i * 8)` is a **signed-int shift**, and
for any `i * 8 >= 24` it shifts a positive signed `int` into the sign
bit or beyond — undefined behavior per C++ pre-14 (and
implementation-defined since), observed in practice as a negative
`int`.  When that negative `int` is implicitly converted to `uint64_t`
for the `APInt` constructor, it is **sign-extended**, so instead of
the intended single-byte mask the `APInt` ends up with *every high
bit set*.

For a 64-bit bswap this turns the byte-3 mask (`0x00000000FF000000`)
into `0xFFFFFFFFFF000000`, which preserves byte 3 as intended **but
also preserves all of bytes 4–7**.  Those unmasked high bytes are
then shifted and OR'd into the final result, contaminating the
bswap output with spurious high-byte contributions.

Any width for which the loop reaches `i >= 4` is affected.  That
means **s64 and wider** when the bug is actually exercised (the loop
runs for `i = 1 .. SizeInBytes/2 - 1`, so s64 reaches `i = 3` only,
but `i * 8 = 24` is enough to trigger the overflow because `0xFF <<
24` already exceeds `INT_MAX`).

## Reproducing the UB in isolation

```cpp
#include <cstdint>
#include <cstdio>
int main() {
  uint64_t v = 0xFF << 24;            // same expression as the APInt ctor arg
  printf("%016llx\n", (unsigned long long)v);
  //   x86-64, g++ 13, -O2: prints  ffffffffff000000
}
```

Compilers will often warn about this if `-Wshift-overflow` /
`-Wsign-compare` are enabled, but the `-Wshift-sign-overflow`
diagnostic for this specific form is not on by default, and the
LLVM build does not trip it (this file compiles warning-clean).

## Why (almost) no one hits it in practice

The short answer: **32-bit GlobalISel targets that matter all narrow
`G_BSWAP` to 32-bit before lowering**, so `lowerBswap` never sees an
s64 input.

Observed rules in the tree today:

| Target    | Rule (paraphrased)                                            | Reaches s64 `lowerBswap`? |
|-----------|---------------------------------------------------------------|---------------------------|
| AArch64   | `legalFor({s32, s64})` (native REV/REV32/REV16)               | no — lowered in isel      |
| X86       | `legalFor({s32, s64})` for BSWAP, SDag/GISel native            | no                        |
| RISC-V    | `BSWAPActions.maxScalar(0, sXLen).lower()`                     | RV32: **narrowed first**; RV64: lowered at s64 where `i*8=24` hits but narrowScalarBswap would not be needed |
| Penumbra (before fix) | `.lowerFor({s32, s64}).widenScalarToNextPow2(0).clampScalar(0, s32, s64)` | **yes — bug triggers** |

On RV64 the bug is latent: the byte-3 mask overflow produces
`0xFFFFFFFFFF000000` but that mask still "works" for a true s64 value
because the register is already 64 bits wide and the extra masked
bytes happen to come from the same `Src`.  Trace through the
algorithm: the extra bits OR'd in from the masked high bytes are
later masked off or OR'd with themselves.  (It would still be wrong
if `SizeInBytes > 8`, e.g. s128, but nobody lowers s128 bswap today.)

On a 32-bit target that narrows *after* `lowerBswap` — our original
Penumbra rule — the s64 mask is split into (lo, hi) halves by the
narrower, and the overflowed-to-all-ones hi half happily makes it
through as `AND reg, -1` instructions, which preserve every high
byte instead of zeroing them.  That's the visible miscompile.

So the bug is:

1. **Real** — the computed `APInt` has the wrong value for any width
   where the loop reaches `i * 8 >= 24`.
2. **Masked on native-bswap targets** — they never invoke
   `lowerBswap` for s64 at all.
3. **Masked on `maxScalar(0, s32).lower()` style rules** — narrowing
   to s32 happens before the lowering, so the loop only reaches
   `i = 1` (for s32, `SizeInBytes = 4`, loop runs `i = 1 .. 1`).
4. **Exposed on rules like `lowerFor({s32, s64})`** — the s64
   lowering runs with the corrupted mask, and any subsequent
   narrowing carries the corruption into real code.

## Symptom

Given input `v = 0x00001234_56789012`, `__builtin_bswap64(v)` should
return `0x12907856_34120000`.  Pre-fix, our codegen returned
`0x12927c56_34120000`.  The diff is isolated to the high register of
the 64-bit return: bytes at positions 1 and 2 of the high word are
OR'd with stray bits (`0x78 → 0x7c`, `0x90 → 0x92`), which trace
back to OR-in contributions from `(v & 0xFFFFFFFFFF000000) << 8`
(intended: `(v & 0xFF000000) << 8`).

## The actual bug in LegalizerHelper.cpp

```cpp
// llvm/lib/CodeGen/GlobalISel/LegalizerHelper.cpp, lowerBswap(), ~line 9716
for (unsigned i = 1; i < SizeInBytes / 2; ++i) {
  // AND with Mask leaves byte i unchanged and sets remaining bytes to 0.
  APInt APMask(SizeInBytes * 8, 0xFF << (i * 8));       // <-- bug here
  ...
}
```

Two possible fixes:

**Minimal**: cast to `uint64_t` before shifting, sidestepping the
`int` intermediate:

```cpp
APInt APMask(SizeInBytes * 8, uint64_t{0xFF} << (i * 8));
```

**Clearer**: use the dedicated APInt builder:

```cpp
APInt APMask = APInt::getBitsSet(SizeInBytes * 8, i * 8, i * 8 + 8);
```

Either would fix the latent-but-real UB and make `lowerFor({s64})`
on 32-bit targets work out of the box.

## What Penumbra did

We chose to match the RISC-V 32-bit convention instead of patching
upstream in our vendored tree — it's the idiomatic shape and yields
better code anyway (two independent s32 bswaps via `narrowScalar`,
merged in reverse, vs a long chain of masked s64 shifts that get
narrowed afterwards).  The Penumbra rule is now:

```cpp
getActionDefinitionsBuilder({G_BSWAP, G_BITREVERSE})
    .lowerFor({s32})
    .widenScalarToNextPow2(0)
    .clampScalar(0, s32, s32);     // narrow s64 -> two s32 bswaps
```

`LegalizerHelper::narrowScalar` for `G_BSWAP` already knows how to
extract parts, bswap each, and merge in reverse — which is exactly
what you want on any 32-bit target.

A lit test covering this is
`llvm/test/CodeGen/Penumbra/bswap64.ll`, and the end-to-end runtime
test is `gcc-c-torture/execute/bswap-1.c` (compares
`__builtin_bswap64` against a hand-rolled union byte shuffle for
eight inputs covering all widths where the bug triggers).

## Upstream status

Not yet reported.  The minimal `uint64_t{0xFF}` change is risk-free
and should be contributed back.

## References

- Penumbra fix: `llvm/llvm/lib/Target/Penumbra/GISel/PenumbraLegalizerInfo.cpp`
  — changed `.lowerFor({s32, s64})` → `.lowerFor({s32})` plus
  `clampScalar(0, s32, s32)`.
- Upstream offender: `llvm/llvm/lib/CodeGen/GlobalISel/LegalizerHelper.cpp`,
  `LegalizerHelper::lowerBswap`.
- Framework narrowScalar path for G_BSWAP (already correct):
  same file, ~line 2051.

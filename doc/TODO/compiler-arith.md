# Compiler: multiply, divide and strength reduction

## Compiler: constant multiply, divide and remainder strength reduction

How constant operands are strength-reduced, and what the cost model
tells the mid-level optimizers, is described in
[`doc/llvm/strength-reduction.md`](../llvm/strength-reduction.md).
The gaps below are open; none of them produces wrong code.

- **The multiply cutoff's exchange rate is untuned.**  1.5 cycles per
  added instruction stands for the I-cache footprint: a miss costs a
  core the same cycles however fast its ALU operations are, so the price
  is absolute rather than a fraction of the multiply, but how often an
  added instruction misses depends on the workload's working set, which
  no single multiply sees.  At 1.5 penumbra1 accepts up to 12
  instructions (13 for a constant needing `LUI`; `x * 0x55555555` is
  12) and penumbra2_5 up to 5 (6).  Random 16-bit constants mostly need
  9-12 and random 32-bit ones 17-20, so the rate mostly decides small
  and structured constants.  Tune on the ULX3S, not the ISS; if the
  cores' measured I-cache miss penalties differ widely, the rate belongs
  to the subtarget.
- **-O0 keeps most constant multiplies.**  No post-legalizer combiner
  runs at -O0, so beyond upstream's power-of-two and -1 rules a constant
  multiply — `x * 3`, even `x * 0` — is a hardware `MUL` there.
- **A 64-bit multiply by a constant keeps a hardware multiply.**  The
  legalizer splits an s64 `G_MUL` by a 32-bit constant `C` into
  partial products: a `G_MUL` and a `G_UMULH` of the low word and `C`,
  and a `G_MUL` of the high word (`C`'s zero high word folds its
  terms).  `penumbra_mul_to_mul_lohi` fuses the low pair into one
  `MULU_P`, and `matchMulByConst` leaves a multiply with a `G_UMULH`
  sibling alone for that reason, so only the high word decomposes.
  `x * 10` on a `uint64_t`, at every tuning:

  ```
  lli  r3, 10
  mulu r1, r3, r3          ; low word, and its carry into the high word
  mov  r4, r2
  shl  r4, 2
  add  r2, r4
  shl  r2, 1               ; high word * 10
  add  r2, r3
  ```

  penumbra1 pays its 37-cycle multiply where the 32-bit `x * 10` is
  four ALU instructions.  Writing `x + (x << 1)` in the source does
  not avoid it: InstCombine turns it back into `x * 3`.  Found in the
  audio-mixing review; reach unmeasured.

  The `G_UMULH` half has no 32-bit decomposition, since the carries
  out of the low word's shifted terms are its input, so the product
  has to decompose whole before the legalizer splits it: a
  pre-legalizer rule, where the s64 `G_MUL` still exists, building s64
  `G_SHL`/`G_ADD`/`G_SUB` for the legalizer to narrow.
  `MulDecomposer` prices its steps in 32-bit instructions (2 for a
  shift and add, 3 with a copy) and needs them per width.  At 64 bits
  an add or subtract is 2 (`ADD`/`ADC`), a copy 2, a shift by one 2
  (`ADD`/`ADC` of itself), by two 4, and by more 5
  (`mov t, lo; shr t, 32−n; shl hi, n; or hi, t; shl lo, n`).  The
  sequence competes with `LLI` (+`LUI`), the `MULU`, the high word's
  own product (decomposed or `MUL`, as the 32-bit rule decides) and
  an `ADD`, at `isMulDecompositionCheaper`'s rate:

  | `uint64_t` product | Now | Decomposed | penumbra1 cycles | penumbra2_5 cycles |
  |---|---:|---:|---:|---:|
  | `x * 3` | 6, one `MULU` | 6 | 47 → 12 | 16 → 6 |
  | `x * 10` | 7, one `MULU` | 10 | 49 → 20 | 17 → 10 |
  | `(uint64_t)u * 10`, `u` 32-bit | 2, one `MULU` | 8 | 39 → 16 | 12 → 8 |

  The last pays only on penumbra1: six added instructions save
  penumbra2_5 four cycles.  Planning it needs the high word known
  zero (a `G_ZEXT` operand), where a shifted term's high word is one
  `SHR` of the low word and an add's is `ADC` of zero.

  The shift-by-one step is a gap on its own: an s64 `x << 1` is six
  instructions today (`mov; shl; shl; shr 31; or; mov`) where
  `add lo, lo; adc hi, hi` does it, so the rule or a custom s64
  `G_SHL` legalization should emit it as `G_UADDO`/`G_UADDE`.
- **One divide answer per type.**  `isIntDivCheap()` sees neither the
  opcode, the signedness nor the constant, so it prices every divide and
  remainder as the costlier remainder shape.  With today's cost points
  that decides identically to an exact answer.  An exact decision is a
  target rule that computes the sequence length from
  `UnsignedDivisionByConstantInfo` / `SignedDivisionByConstantInfo`,
  compares it against the model's latencies, and calls
  `CombinerHelper::applyUDivOrURemByConst` / `applySDivOrSRemByConst` —
  the hook then stays true.
- **64-bit divides keep their libcall.**  `G_SMULH` s64 does not
  legalize, and the hook cannot tell signed from unsigned.  `G_UMULH`
  s64 does (s32 partial products), so a per-instruction rule could take
  unsigned 64-bit divides away from `__udivdi3`; legalizing `G_SMULH`
  s64 unlocks the signed case.
- **A quotient and remainder of the same non-power-of-two divisor stay
  one `DIV_P`.**  `div_rem_to_divrem` fuses the pair and the magic rules
  match only the separate opcodes.  A rule shaped like
  `penumbra_srem_pow2` — quotient through the rewrite, remainder as
  `x - q * C` — would take it.
- **Narrow signed divide sequences carry redundant sign extensions.**
  The widened high-half product is sign-extended from its doubled width
  although it already fits, e.g. in `sdiv i8 %x, -3`.
- **The signed power-of-two remainder clears bits the long way.**
  `penumbra_srem_pow2` rounds the biased dividend down with `SAR` then
  `SHL`, because `-2^k` does not fit the uimm16 `ANDi` takes: 7
  instructions for `x % 8`.  Masking the remainder out instead keeps
  the bias live and needs `2^k - 1`, which fits for `k <= 16` —
  `((x + bias) & (2^k - 1)) - bias`, 6 instructions, and identical to C
  truncation for every `s32` including `INT_MIN` (checked for every `k`
  up to 16).  Above 16 the mask needs loading and the shift form stays
  shorter.  A fused quotient and remainder could share the bias.  The
  fix is local to `applySRemByPow2`.

## Compiler: a hoisted shift stops a chained fixed-point multiply fusing

Found 2026-09-22 while inlining Doom's `FixedMul`.  Where one
fixed-point multiply feeds another — `FixedMul(FixedMul(a,b),c)`, as
`R_MapPlane` computes `distance` and then `ds_xstep`/`ds_ystep` —
InstCombine removes the `sext(trunc(...))` round trip between them by
hoisting the shift into the first multiply:

```llvm
%9  = shl nsw i64 %6, 16     ; sext(planeheight) << 16
%10 = mul i64 %9, %7         ; x sext(yslope[y])  -- genuinely 48x32
%11 = ashr i64 %10, 32       ; the fixed-point result, still i64
%13 = mul nsw i64 %11, %12   ; this one fuses
```

The first multiply of a chain is then not a 32×32 widening multiply in
any form the hardware has, so it takes the schoolbook expansion: two
extra hardware multiplies and about seven instructions of sign
correction.  Only the head of each chain is affected; the rest fuse,
because `ashr` by 32 leaves a value `GISelValueTracking` can prove is
32-bit.  The transform is right on a machine with a 64-bit multiplier,
so this is a target gap, not an InstCombine bug.

Scope, counted statically: Doom 8 sites (only `R_MapPlane` executes,
207 times a frame), kernel 9, `libc` 3, `libsqlite3` 1.  At 2 extra
multiplies and ~7 instructions each, Doom pays roughly 5k cycles a
frame, about 0.05%.

Three ways to close it, none of them earning their keep at that price:

- **A combine that sinks the shift back out**, `(X << C) * Y >> D` →
  `(X * Y) >> (D - C)`, is the general fix and would reach the kernel
  and `libc` sites too.  The two forms agree only in the low 64 − C
  bits, so it needs a demanded-bits argument; the `nsw` that would make
  it unconditional is what InstCombine drops.
- **Writing the extraction explicitly** in `m_fixed.h`,
  `((uint32_t) p >> 16) | ((uint32_t) (p >> 32) << 16)` — keeps the
  value in 32-bit terms, so nothing is hoisted: the three-multiply
  chain measures 31 instructions and 3 multiplies against 37 and 5.  It
  is a source workaround for a compiler canonicalization, and it leaves
  a redundant `and` of `0xFFFF0000` behind.
- Leave it, which is the current answer.

What would change the calculus is a workload that is chained fixed
point throughout: the 32.32 audio mixer is the candidate, so look for
this shape when that path is next profiled.

## Ruled out

- **Folding the subtracting decomposition's extra copy.**  A
  `2^n - 1`-shaped product that must land in the multiplicand's
  register (`return x * 7`) costs a fourth instruction, a copy back.
  The combine cannot emit the copy itself (it is coalesced straight
  back); the fix belongs to two-address lowering or a post-RA
  peephole.  Across `make test-compiler` (~42M instructions) it occurs
  16 times, all right before a return, 12 of them in `mul-const.c`
  itself — not worth a new pass.
- **Negation as `NOT` + `ADD 1`.**  `0 - x` in the operand's own
  register is three instructions where `not; add 1` is two, with
  identical NZCV.  A survey of the kernel, libc and 18 userland
  binaries (2.2M instructions) found 687 negations and the copy-back
  in about 36.  The `p - n` pointer case, the one with reach, is
  handled by `penumbra_ptr_add_neg_to_sub`
  ([write-up](../llvm/gisel-pointer-minus-integer.md)).

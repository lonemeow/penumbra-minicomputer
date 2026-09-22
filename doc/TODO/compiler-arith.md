# Compiler: multiply, divide and strength reduction

## Compiler: G_ZEXT s128 from the mulhi idiom fails to legalize — RESOLVED

A function that computes the high 64 bits of a 64×64 product from
32-bit partial products and returns only that half (the classic
mulhi shape) gets idiom-recognized by AggressiveInstCombine's
ungated `foldMulHigh` into `zext i64 → i128; mul i128; lshr 64;
trunc`, which ICEd the legalizer.  C23 `_BitInt(128)` reached the
same gap directly.  (`compiler-rt`'s `wideMultiply` escapes
recognition because it returns both halves through out-params; the
32→64 flavor of the same transform is a win for us — a hand-written
`mulhi32` becomes a single MULU.)

Fixed in three parts, no actual i128 support needed (GISel narrows
iteratively s128 → s64 → s32 through the same paths s64 uses):
extension narrowing widened from `typeIs(s64)` to
`scalarWiderThan(32)`; extending loads with memory wider than
register width (`G_ZEXTLOAD s128 ← s64`, formed when a load feeds
the zext) lowered to plain load + extension; and the missing
aligned-pow-2 case added to the generic
`LegalizerHelper::lowerLoad` (it previously assumed lower() on an
extload always meant an unaligned split).  Test:
`test/CodeGen/Penumbra/i128-mulhi.ll`.

**Upstream-worthy:** the `lowerLoad` gap is generic — riscv32
GlobalISel ICEs on the identical `G_ZEXTLOAD s128 ← s64` from the
same IR (verified against an in-tree RISCV llc build), via the
sibling `// FIXME: Need to split the load.` hole in
`narrowScalar`'s extload case.  Full upstream-facing analysis,
reproducers, target survey, and proposed two-part patch:
`doc/llvm-gisel-wide-extload-legalization.md`.

## Compiler: constant multiply, divide and remainder strength reduction

`MUL` and `DIV` execute on the sequential `divmul` peer unit and hold
the core many times longer than an ALU operation — the per-core figures
are in [instruction-timing.md](internals/instruction-timing.md) and in
each core's scheduling model — so constant operands decompose into ALU
operations wherever that is cheaper on the tuned core.

### What exists

- **Multiply.**  `penumbra_mul_by_const` in the post-legalizer combiner
  rewrites an s32 multiply by a constant as shifts, adds and subtracts
  of the multiplicand.  `MulDecomposer` searches for the shortest
  sequence on the two-address machine, copies included: terms
  accumulated one at a time (Horner's rule, so the running sum is always
  the operand overwritten and a subtract costs no more than an add),
  factors of `2^n +/- 1` (`x * 0x01010101` is `257 * 65537`, 6
  instructions), a negative product built on `0 - x`, and a final
  `x - t` in the multiplicand's own register when the multiply is its
  last use.  `isMulDecompositionCheaper` takes the sequence where every
  instruction it adds over the constant load and `MUL` saves at least
  1.5 of the model's cycles, and at minsize where it is no longer.  A
  multiply whose operands also feed a `G_[SU]MULH` is left to fuse into
  one `MUL_P`/`MULU_P`.  The legalizer keeps only the s64 path (a 32x32->64
  widening multiply as one hardware multiply); s32 `G_MUL` is legal.
  Upstream's `mul_to_shl` / `mul_by_neg_one` cover powers of two and -1
  before legalization, at every optimization level.
- **Divide or remainder by a power of two.**  The legalizer lowers
  unsigned `G_UDIV` / `G_UREM` (so these reduce even at -O0); upstream's
  `sdiv_by_pow2` covers the signed divide, and `penumbra_srem_pow2` the
  signed remainder (see
  [the upstream-candidate write-up](llvm-gisel-signed-pow2-remainder.md)).
- **Divide or remainder by any other constant.**  Upstream's
  `*_by_const` magic-number rules, gated by `isIntDivCheap()`, which
  keeps the divide under minsize, above 32 bits, and where the tuned
  core's divide does not cost more than two multiplies plus a few ALU
  operations.  Narrow high-half multiplies from i8/i16 divides lower
  through a multiply at twice the width.

Covered by `mul-by-const.ll`, `div-by-const.ll`, `srem-pow2.ll`,
`strength-reduce.ll`, `mtune.ll` and the `penumbra-codegen/`
execution tests (`mul-const*.c`, `div-rem-const*.c`,
`sdiv-srem-pow2.c`).

### Known gaps

None of these produce wrong code.

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
  | `x * 3` | 6, one `MULU` | 6 | 47 → 12 | 15 → 6 |
  | `x * 10` | 7, one `MULU` | 10 | 49 → 20 | 16 → 10 |
  | `(uint64_t)u * 10`, `u` 32-bit | 2, one `MULU` | 8 | 39 → 16 | 11 → 8 |

  The last pays only on penumbra1: six added instructions save
  penumbra2_5 three cycles.  Planning it needs the high word known
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
- **A subtracting decomposition can cost an extra copy.**  A subtract
  does not commute, so its result stays in the register holding the
  shifted term, and that register cannot be the multiplicand's own
  while the multiplicand is live across the shift.  When the product
  is consumed elsewhere this costs nothing — `x * 7` stored to memory
  is `mov r3, r1; shl r3, 3; sub r3, r1` — but when it must land in
  the multiplicand's register (a return, or an operation tied there)
  a fourth instruction copies it back: returned, `x * 7` is
  `mov r2, r1; shl r2, 3; sub r2, r1; mov r1, r2`, where
  `mov r2, r1; shl r1, 3; sub r1, r2` is three.  The rule cannot fix
  this by emitting the copy itself: an explicit copy of the
  multiplicand ahead of the shift is coalesced straight back to the
  same four instructions.  It belongs to two-address lowering, which
  copies the shift's source rather than the subtract's other operand,
  or to the coalescer; a post-register-allocation peephole with a
  dead-register check would also catch it.  Every `2^n - 1` shape and
  subtracting constant is exposed (`* 7`, `* 240`, `* 255`), but the
  copy is rare in practice: across the compiled `make test-compiler`
  suite (~42M instructions) it appears only immediately before a
  return, 16 times, 12 of them in `penumbra-codegen/mul-const.c`
  itself, and never mid-function with the scratch register dead.
  That does not justify a new pass; revisit if more placement-level
  waste turns up.
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
- **Negation can take three instructions where two do.**  `0 - x`
  selects to `SUB` from a copy of R0, so when the result must land in
  the operand's register it is `mov r2, zero; sub r2, r1; mov r1, r2`,
  where `not r1, r1; add r1, 1` is two — and `NOT` takes a separate
  source, so the pair is two wherever the result lands.  The pair sets
  identical NZCV: SUB's carry is NOT borrow, so both set C only for
  zero and V only for `INT_MIN`.  The fix would be one selection
  pattern, `(sub 0, x)` → `ADDi (NOT x), 1`, but it is worth little.
  A survey of the rebuilt kernel, libc and 18 userland binaries (2.2M
  instructions) finds 687 negations, 0.03% of instructions, and the
  copy-back in only about 36 of them.  By first consumer, the
  negations are stored, copied, compared, passed to calls or unused
  nearby (386), masked as `x & -m` roundups with no cheaper form (79),
  or feed an add, directly or shifted (204).  Integer `a + -b` already
  folds in the IR, so the adds are C's `p - n`, which reaches the
  backend as `p + (0 - n)` and cost a zero copy, a subtract and an add,
  plus any scaling, where a subtract does.  The post-legalizer rule
  `penumbra_ptr_add_neg_to_sub` now rewrites it (see
  [the upstream-candidate write-up](llvm-gisel-pointer-minus-integer.md));
  the sites are per-packet mbuf adjustments in protocol input and output
  and driver ring pointers.  Negative constant multiplies do not need
  the pattern either: the decomposition accumulates from `0 - x` while
  the multiplicand is still live, so an in-place negation would not
  shorten `x * -80`.

### Cost model

`PenumbraTTIImpl::getArithmeticInstrCost` prices `Mul` and
`SDiv`/`UDiv`/`SRem`/`URem` in ALU operations — the tuned core's
multiply or divide cycles over its ALU cycles, rounded up — for
throughput and latency, scaled by the legalization factor, and leaves
code size to the base model: one instruction is emitted however long
it runs, and answering otherwise would distort inlining and unrolling.
Covered by `test/Analysis/CostModel/Penumbra/arith.ll`.  The cost model
corrects what LSR and the unroller see, but InstCombine canonicalises
without consulting it, which is why the multiply rewrite has to live in
the backend.

## Compiler: MUL is not commutable, so a live multiplicand is copied — RESOLVED

`MUL` and `MULU` are destructive (`Rd = Rd × Rs`) and commutative,
but unlike `ADD`, `AND`, `OR` and `XOR` they are not marked
`isCommutable` in `PenumbraInstrInfo.td`.  They share the
`$Rd = $Rd_in` block with `DIV` and `DIVU`, which must not commute.
Two-address lowering therefore ties the result to the operand that
comes first in the IR, and when that operand is still live it is
copied first — the `MOV` the comment on the `ADD` group describes.

Whether it bites depends on InstCombine's operand order.  `p[i] * k`
in a loop is `ldw r11, [r1+0]; mul r11, r2`: the dead operand comes
first and nothing is copied.  The audio mixer's product is ordered
`vol * s`, so each voice-sample pays `mov r3, r7; mul r3, r9` to keep
the loop-invariant volume.  Reduced:

```c
int f(int a, int b) { return a * b + a; }    /* a * (b + 1) */
```

is `add r2, 1; mul r2, r1; mov r1, r2`, where `add r2, 1;
mul r1, r2` is enough.

Resolved by marking `MUL`, `MULU` and the pair forms `MUL_P` and
`MULU_P` commutable — swapping the sources changes neither half of
the product — with `DIV`, `DIVU` and their pair forms left as they
are.  The mixer's multiply is `mul r8, lr` with no copy, 16
instructions per voice-sample where it was 17, and each
divide-by-a-constant's magic multiply loses its copy
(`test/CodeGen/Penumbra/mul.ll`, `div-by-const.ll`).

One case got worse: in `mul-by-const.ll`'s `mul64_minsize`, two-address
lowering now commutes rather than moving the multiply below the last
use of its operand, so the product lands in the constant's register and
needs a copy to the return register.  It is the same two-address
heuristic as "a commutable `ADD` still copies into an argument
register" in the audio-mixing review.

## Compiler: fuse a widening multiply into a single MUL_P — RESOLVED

A 32×32→64 widening multiply currently selects to **two** hardware
multiplies (commit `ba7ef55`): the custom legalization of `G_MUL s64`
emits `G_MUL(a,b)` for the low word and `G_S/UMULH(a,b)` for the high,
which select to `MUL`+`MUL_P` (signed) or `MUL`+`MULU_P` (unsigned).
But `MUL_P`/`MULU_P` already produce **both** halves in one instruction,
so the separate low `MUL` is redundant — it recomputes the low product
that `MUL_P` discards into a dead register.  Collapsing the pair to a
single `MUL_P`/`MULU_P` is the remaining optimization.

**Measured leverage (ULX3S @ 25 MHz, mandelbrot seahorse, µs/sample):**
56141 (software `__muldi3`) → 4244 (4-mul schoolbook, `beb685b`) →
2584 (2-mul pair, `ba7ef55`).  Fitting `cost = k·muls + N` to the last
two points gives `k≈830`, `N≈924`, so a 1-mul form projects to
**~1750 µs/sample — a further ~1.47×, ~32× over the software baseline.**
Diminishing: the non-multiply floor (`>>28` realign + escape loop) is
already ~36% of the sample, so the third multiply matters less than the
first two did.  Only multiply-bound fixed-point code (this demo) sees
the full benefit; the kernel and Dhrystone are not multiply-bound.

**Why it's deferred — no clean GISel mechanism exists.**  GISel has no
two-result multiply op (SelectionDAG's `ISD::SMUL_LOHI`/`UMUL_LOHI` have
no `G_*` equivalent), so a single instruction producing both halves can't
be expressed at the generic level.  Prior art doesn't transfer:
- **ARM** (`SMULL`/`UMULL`, our exact twin) forms it via SelectionDAG's
  `SMUL_LOHI` — SelectionDAG only.
- **x86** (`MUL`→`EDX:EAX`) selects each half to a full `MUL` reading the
  fixed result register, then leans on `MachineCSE` to fold the two
  identical instructions into one.  Needs **fixed** output registers;
  our `MUL_P` writes regalloc-chosen vregs, so two `MUL_P`s never CSE.
- **RISC-V RV32** does the same 2-instruction schoolbook we do now and is
  content with it — its `mul`/`mulh` are genuinely two instructions, so
  it has no single-instruction form to fuse toward.

**Implementation routes, both with real cost:**
1. Selection-time pairing in `selectDivMul`: when selecting `G_S/UMULH`,
   scan for a sibling `G_MUL` with identical operands, emit one `MUL_P`,
   route its low output to the `G_MUL` result and erase it.  Order-
   sensitive (either op may be selected first) — the fiddly part.
2. Build `MUL_P` directly in the custom legalization (skip the generic
   pair entirely).  Bypasses the fusion but emits a target instruction
   pre-RegBankSelect, which is non-idiomatic and needs manual reg-class
   constraining.

Not worth the complexity until something multiply-bound matters more than
the ~1.47× on a demo.  Correctness surface if revisited: the same
signed/unsigned/i64 widening + `smul`/`umul` overflow execution tests used
for `ba7ef55`.

**2026-08: the deferral gate has flipped.**  Enabling `BN_LLONG` in the
OpenSSL build (`f0942b3981a9`) made `bn_mul_add_words` — one widening
multiply per limb product — the RSA/DH critical path, and it now runs
the unfused `MUL`+`MULU` pair (~66 divmul cycles where 33 suffice) on
real workloads: RSA sign, DH kex, ssh.  Worse, the compiled loop shows a
**third** multiply per limb: an unfolded multiply-by-known-zero cross
term from the 64×64 narrowing (`mul r11, lr` with `lr` holding a
loop-carried zero, plus a dead `add r7, 0`) that the minimal
`acc + (u64)a*b` repro does *not* reproduce — the zero flows through the
unrolled loop's phis and escapes the constant folds.  Fixing fusion
alone roughly halves divmul time in all bignum inner loops; chasing the
zero-term as well brings ~3× on the multiply component.  Estimated
~1.5–2× on RSA sign end-to-end, on top of the BN_LLONG 1.5–1.8×.

**Fixed via a third route the survey above missed: a target *generic*
opcode.**  Neither selection-time pairing nor pre-RegBankSelect target
instructions were needed — GISel lets a target define its own `G_*`
opcodes (AArch64's `G_ADD_LOW` etc.), which is exactly the missing
"two-result multiply" the generic opcode set lacks.
`PenumbraInstrGISel.td` defines `G_UMUL_LOHI`/`G_SMUL_LOHI`; the
post-legalizer rule `penumbra_mul_to_mul_lohi` fuses a same-operand
`G_MUL` + `G_[SU]MULH` pair (either operand order) into one, and the
selector emits `MUL_P`/`MULU_P`.  Everything stays on the normal
pipeline, so the fiddly selection-order problem never arises.

Gotcha for the next target generic opcode: `isPreISelGenericOpcode()`
covers only the shared `G_*` range, so the selector's entry guard has to
use `MachineInstr::isPreISelOpcode()` — otherwise target generic opcodes
are taken for already-selected instructions and reach the register
allocator unselected (crash in the coalescer, not a clean verifier
error).

Measured statically: 64×64 multiply 4 multiplies → 3; widening 32×32→64
2 → 1; the fractal demos' Q4.28 inner loop 6 → 3.  Divide/remainder was
never affected — `G_SDIVREM`/`G_UDIVREM` already exist generically and
the legalizer's `div_rem_to_divrem` fuses those upstream.

**An operand from memory missed it until 2026-09-22** (`2dbdf66`).  The
custom lowering recognised the widening form by matching `G_SEXT` /
`G_ZEXT` on both operands, which only holds for a value extended in a
register: a value loaded from memory arrives as one extending load,
because the combiner folds `G_SEXT (G_LOAD s32)` into `G_SEXTLOAD`
first.  Those multiplies fell back to the schoolbook expansion — three
hardware multiplies and the sign corrections between them.  The test is
now whether an operand carries information above bit 31, asked of
`GISelValueTracking`, so extending loads and values already narrowed to
fixed point qualify without naming either shape.  Static sites carrying
the expansion before the fix: kernel 63, `libc` 39, `libsqlite3` 26,
and every inlined `FixedMul` in Doom.  What remains unfused is the
chain head — see "a hoisted shift stops a chained fixed-point multiply
fusing".

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

## Compiler: the pipeline prices MUL/DIV as if they cost one cycle — RESOLVED

`PenumbraTTIImpl::getArithmeticInstrCost` prices them at the tuned
core's scheduling-model latencies, and `isIntDivCheap()` weighs the
same figures (see
[the constant strength-reduction entry](#compiler-constant-multiply-divide-and-remainder-strength-reduction)).
The analysis below predates both.  Its closing claim that
`isIntDivCheap()` should stay true held only while every core's
multiply was as slow as its divide.

The divmul unit runs a fixed 32-iteration shift-add — no early-out, so
every `MUL`/`DIV` is a full multi-cycle stall regardless of operand
values.  Nothing in the compiler knows this.  `PenumbraTTIImpl` does not
override `getArithmeticInstrCost`, so `BasicTTIImplBase` reports the
same cost as an `ADD` for any legal operation, and the scheduling model
(`CompleteModel = 0`, no `WriteRes` entries) models the same.  Every
cost-driven decision — unrolling, LSR, SimplifyCFG speculation,
inlining, if-conversion — is therefore made believing a divmul is free.

Overriding `getArithmeticInstrCost` for s32 MUL/DIV/REM is the small,
broad fix.  Report the real cost for `TCK_Latency` and
`TCK_RecipThroughput` but leave `TCK_CodeSize` at 1: a `MUL` genuinely
*is* one instruction, and inflating its code-size cost would push the
unroller and inliner the wrong way on a core that already spends ~34%
of fractal-demo cycles stalled on instruction fetch.

`isIntDivCheap()` — the one hook that *is* set — remains correct, and
should not be flipped when the cost model lands.  Magic-multiply
division needs only the high half, so it is one `MULU_P`: the same
divmul latency as the `DIVU` it would replace, plus fixup shifts.
Modulo is worse, needing the divide plus a multiply-back and subtract.
The divide side is a genuine wash because our multiply is exactly as
slow as our divide; the asymmetry is entirely on the multiply side.

## Compiler: constant multiplies below the 2^n +/- 1 cases hit the divmul unit — RESOLVED

The post-legalizer `penumbra_mul_by_const` rule decomposes constant
multiplies against the tuned core's costs; see
[the constant strength-reduction entry](#compiler-constant-multiply-divide-and-remainder-strength-reduction).
The analysis below predates it.

Custom strength reduction in `PenumbraLegalizerInfo` covers `2^n` and
`2^n +/- 1` (so `x*3`, `x*5` become shift+add) and falls through to a
hardware `MUL` for everything else.  That cap is the right one for a
target whose multiply costs a few cycles — which is what the rule was
calibrated against, before the divmul unit existed — but here the
budget for replacing a constant multiply is the unit's full latency:
roughly 11 instructions even in a fetch-bound loop, more when fetch is
warm.  We spend at most two.

Missed cases are ordinary: `x*6`, `x*10`, `x*12`, `x*30`, `x*100` all
emit `MUL` where three to five shift/add/sub instructions suffice.
`x*320` is the one to note — a framebuffer row stride, so any
`fb[y*320+x]` that LSR does not convert into an induction variable
(`putpixel`, random-access blits) pays a full divmul stall per call.

A greedy shift/add decomposition over the constant (canonical signed
digit form handles the `-` cases like `30 = 32-2`) bounded at a few
instructions would cover essentially every small constant in real code.
Two traps for whoever picks this up: `TargetLowering::
decomposeMulByConstant` looks like the intended hook and is implemented
by RISC-V, X86, M68k, PowerPC, LoongArch, CSKY and Xtensa, but it is
called only from `DAGCombiner` and is dead under GlobalISel; and GISel's
own upstream coverage is just `mul_to_shl` (power-of-2), so there is no
rule to enable.  The work belongs in the existing custom MUL
legalization next to the `2^n +/- 1` cases, or in a post-legalizer
combine.

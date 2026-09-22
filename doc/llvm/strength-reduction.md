# Constant multiply, divide and remainder strength reduction

`MUL` and `DIV` execute on the sequential `divmul` peer unit and hold
the core many times longer than an ALU operation — the per-core figures
are in [instruction-timing.md](../internals/instruction-timing.md) and in
each core's scheduling model — so constant operands decompose into ALU
operations wherever that is cheaper on the tuned core.  Open gaps are
tracked in
[`doc/TODO/compiler-arith.md`](../TODO/compiler-arith.md#compiler-constant-multiply-divide-and-remainder-strength-reduction).

## Lowerings

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
  [the upstream-candidate write-up](gisel-signed-pow2-remainder.md)).
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

## Cost model

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

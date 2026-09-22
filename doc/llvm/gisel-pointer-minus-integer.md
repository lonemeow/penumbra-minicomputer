# LLVM GlobalISel keeps the negation in pointer minus integer — `p - n` becomes `p + (0 - n)`

## TL;DR

LLVM IR has no pointer subtract, so C's `p - n` reaches code
generation as a `getelementptr` whose index is `sub 0, n`.
SelectionDAG lowers the offset to plain integer nodes, where
`DAGCombiner` folds `A + (0 - B)` to `A - B` and
`A + ((0 - B) << C)` to `A - (B << C)`.  GlobalISel keeps pointer
arithmetic as `G_PTR_ADD`, and its `simplify_add_to_sub` rule matches
only `G_ADD`, so the negation survives into the output: one extra
instruction on riscv32, and on a two-address target a zero copy,
a subtract and an add where one subtract does.

Integer `a + (0 - n)` folds in both pipelines; only the pointer form
diverges.

## Reproducer

```llvm
define ptr @bytes(ptr %p, i32 %n) {
  %neg = sub i32 0, %n
  %r = getelementptr inbounds i8, ptr %p, i32 %neg
  ret ptr %r
}
define ptr @words(ptr %p, i32 %n) {
  %neg = sub i32 0, %n
  %r = getelementptr inbounds i32, ptr %p, i32 %neg
  ret ptr %r
}
define i32 @ints(i32 %a, i32 %n) {
  %neg = sub i32 0, %n
  %r = add i32 %a, %neg
  ret i32 %r
}
```

```
llc -mtriple=riscv32 -mattr=+m [-global-isel]
```

| function | SelectionDAG | GlobalISel |
|----------|--------------|------------|
| `bytes`  | `sub` | **`neg`** `add` |
| `words`  | `slli` `sub` | **`neg`** `slli` `add` |
| `ints`   | `sub` | `sub` |

Clang emits exactly this IR for `p - n` at `-O2`; InstCombine does
not, and cannot, remove the negation, since the only pointer
arithmetic IR has is a `getelementptr` offset.

Verified against LLVM 22.1.2 (this tree's base).

## How each pipeline reaches its answer

**SelectionDAG.**  `SelectionDAGBuilder::visitGetElementPtr` lowers
each index to `ISD::MUL` or `ISD::SHL` by the element size and
`ISD::ADD` onto the pointer, all on the pointer-sized integer type.
`DAGCombiner::visitADDLike` then folds `(A + (0-B)) -> A-B`, and
`visitADDLikeCommutative` folds
`(add x, shl(0 - y, n)) -> sub(x, shl(y, n))`.

**GlobalISel.**  `IRTranslator::translateGetElementPtr` scales the
index with `G_MUL` (a power of two later becomes `G_SHL` through
`mul_to_shl`) and adds it with `G_PTR_ADD`.  `simplify_add_to_sub`,
`CombinerHelper::matchSimplifyAddToSub`, is the GlobalISel
counterpart of the first fold, but its rule matches `G_ADD` only.
There is no counterpart of the second.  GlobalISel has no pointer
subtract to rewrite to, which is presumably why neither rule was
extended to `G_PTR_ADD`.

## Proposed fix

Rewrite `G_PTR_ADD p, (0 - n)` as a subtract through the pointer
casts, `G_INTTOPTR (G_SUB (G_PTRTOINT p), n)`, and look through one
scaling of the negation, since `(0 - n) << k` and `(0 - n) * k` are
both `0 - (n op k)` in modular arithmetic:

- **Unscaled:** always profitable; a negation with other uses stays
  for them and costs nothing more than the add did.
- **Scaled:** only when the scaling and the negation both have no
  other use (otherwise the scaling is computed twice), and only when
  the scaling is in the add's block (it is rebuilt at the add, which
  would otherwise move a loop-invariant stride into the loop).

On a target that also rewrites multiplies by constants, the combiner
visits a scaling `G_MUL` before its `G_PTR_ADD`, so that rewrite has to
leave such a multiply for this one.

Two points for review:

- **The casts.**  `G_PTRTOINT`/`G_INTTOPTR` are legal and select to
  copies on targets whose pointers are plain integers, but not on
  every target; non-integral address spaces must be excluded, and a
  target may prefer the fold at selection instead, as a
  `(ptradd p, (sub 0, n))` pattern.
- **Where it runs.**  Pre-legalization it reaches every target; a
  post-legalization copy would need the same cast legality check.

Testable with a combiner MIR test plus `llc` on the IR above under
both pipelines.

## Status in this tree

Penumbra carries the rule locally as `penumbra_ptr_add_neg_to_sub` in
its post-legalizer combiner, where the casts are always legal and
select to copies.  Its constant-multiply rule, which the combiner
reaches first, leaves a negated multiplicand that scales a pointer
offset in the same block to it.

On Penumbra's two-address instruction set the negation costs more
than on riscv32: `p - n` was `mov t, zero; sub t, n; add p, t` and is
now `sub p, n`.  In a NetBSD kernel, libc and userland sample
(2.2M instructions) about 200 such sites — per-packet mbuf adjustments
in protocol input and output, driver ring pointers — lose one or two
instructions each.  The survey is in
[the strength-reduction entry](../TODO/compiler-arith.md#compiler-constant-multiply-divide-and-remainder-strength-reduction).

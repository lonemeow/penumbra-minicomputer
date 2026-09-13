# LLVM GlobalISel has no power-of-two signed remainder lowering — `x % 4` becomes a magic multiply or a hardware divide

## TL;DR

SelectionDAG lowers `srem x, 2^k` to a short multiply-free sequence.
GlobalISel has no equivalent: `intrem_combines` in
`llvm/include/llvm/Target/GlobalISel/Combine.td` contains only
`urem_by_const` and `srem_by_const`, and `srem_by_const` sends
**every** constant divisor, powers of two included, through
`CombinerHelper::buildSDivOrSRemUsingMul`.  What a GlobalISel target
gets for signed `x % 4` depends only on `isIntDivCheap()`:

- **hook false** — the magic-number sequence, high-half multiply
  and all, for a divisor that needs none: nine instructions on
  riscv32 where SelectionDAG emits five ALU operations.
- **hook true** — a hardware remainder instruction.

The multiply-free expansion GlobalISel needs is already in the same
file: `sdiv_by_pow2` lowers `sdiv x, 2^k` without a multiply, and
`urem_pow2_to_mask` handles the unsigned remainder.  Only the signed
remainder was left without a power-of-two rule.

A second, smaller divergence sits alongside it: the two pipelines
gate the power-of-two **divide** differently, so under `minsize` the
same IR yields a `div` from one and a shift sequence from the other.

## Reproducer

```llvm
define i32 @srem4(i32 %x) { %r = srem i32 %x, 4
  ret i32 %r }
define i32 @urem4(i32 %x) { %r = urem i32 %x, 4
  ret i32 %r }
define i32 @sdiv4(i32 %x) { %r = sdiv i32 %x, 4
  ret i32 %r }
```

```
llc -mtriple=riscv32 -mattr=+m [-global-isel]
```

riscv32 answers `isIntDivCheap()` true exactly under `minsize`, so
adding the attribute to each function toggles the hook on an in-tree
target with no custom lowering.

**Without `minsize`** (hook false):

| function | SelectionDAG | GlobalISel |
|----------|--------------|------------|
| `srem4`  | `srai` `srli` `add` `andi -4` `sub` | `lui` `addi` **`mulh`** `add` `srai` `srli` `add` `slli` `sub` |
| `urem4`  | `andi 3` | `andi 3` |
| `sdiv4`  | `srai` `srli` `add` `srai` | `srai` `srli` `add` `srai` |

**With `minsize`** (hook true):

| function | SelectionDAG | GlobalISel |
|----------|--------------|------------|
| `srem4`  | `li 4` `rem` | `li 4` `rem` |
| `urem4`  | `andi 3` | `andi 3` |
| `sdiv4`  | `li 4` **`div`** | `srai` `srli` `add` `srai` |

Verified against LLVM 22.1.2 (this tree's base).

## How each pipeline reaches its answer

**SelectionDAG.**  `DAGCombiner::visitREM` folds `urem x, 2^k` to a
mask unconditionally.  For `srem`, it enters a block guarded by
`!TLI.isIntDivCheap(VT, Attr)` and tries `buildOptimizedSREM` first,
which calls the `TargetLowering::BuildSREMPow2` customization point.
When the target supplies nothing, it rewrites `X % C` as
`X - (X / C) * C` through `visitSDIVLike`.  Because the divisor is a
power of two, that divide lowers through the power-of-two path, the
multiply by `C` becomes a shift, and the result folds to the
multiply-free form in the first table.

The default implementations of `BuildSDIVPow2` and `BuildSREMPow2`
in `TargetLowering.cpp` also consult the hook, returning the node
itself ("Lower SDIV as SDIV") when it is true.  That is why
SelectionDAG keeps a hardware `div` for `sdiv4` under `minsize`.

**GlobalISel.**  `urem_pow2_to_mask` folds the unsigned case
unconditionally.  `sdiv_by_pow2` calls `matchDivByPow2`, which
consults no hook, so the power-of-two divide expands even under
`minsize`.  There is no power-of-two signed remainder rule.
`srem_by_const` goes through `matchSDivOrSRemByConst`, which returns
early when the hook is true and otherwise applies
`buildSDivOrSRemUsingMul` — the general magic-number construction,
applied to a power of two it never needed.

The fix direction is visible in the rule set itself:
`X - ((X sdiv 2^k) << k)` with the divide built by the existing
`sdiv_by_pow2` expansion is exactly SelectionDAG's result, and uses
nothing GlobalISel does not already have.

## What `isIntDivCheap()` is asked, and what it can answer

The hook's documented contract, in `TargetLowering.h`:

> Return true if integer divide is usually cheaper than a sequence of
> several shifts, adds, and multiplies for this target.  The
> definition of "cheaper" may depend on whether we're optimizing for
> speed or for size.

Every upstream override that ever returns true does so for size:

| target | returns | stated rationale |
|--------|---------|------------------|
| AArch64, X86, RISC-V | `MinSize && !isVector()` (RISC-V also bounds the width) | a div instruction is usually smaller than the alternative sequence |
| WebAssembly | `true` | engines perform the optimization, so save code size |
| BPF, VE | `false` | — |

The contract equally permits a speed answer, and that is where one
boolean runs out.  Generic code asks it about sequences of different
composition:

- the **magic-number** sequence contains a high-half multiply;
- the **power-of-two** divide and remainder sequences contain only
  shifts, adds and masks.

A target whose multiply is as slow as its divide — both iterating
on one sequential unit, say — can truthfully answer true for the
first and false for the second.  One answer cannot cover both.  The
size-driven overrides never notice, because at `MinSize` a single
divide instruction beats either sequence.

## Existing mechanisms for relative operation cost

LLVM does carry machinery for comparing an operation against a
decomposition; none of it answers the division question for a
GlobalISel target.

- **`TargetLowering::decomposeMulByConstant`** asks whether a
  multiply by a constant should become shifts and adds, explicitly
  including when "the sequence of simpler ops is faster than the
  multiply".  Seven targets implement it (CSKY, LoongArch, M68k,
  PowerPC, RISC-V, X86, Xtensa).  Its only caller in `lib/CodeGen` is
  `DAGCombiner::visitMUL`, which performs the `2^N ± 1` and
  `2^N ± 2^M` decompositions.  The hook takes the constant as an
  `SDValue`, so GlobalISel cannot call it as declared; GlobalISel has
  no equivalent rule or hook.
- **`MachineCombiner`** compares a candidate instruction sequence
  against the original by critical-path latency from the target's
  scheduling model (`TargetSchedModel::computeInstrLatency`), and
  does nothing for a target without an instruction-level model.  Its
  patterns are target-supplied.

Both concern multiplication or target-specific rewrites.  Nothing
compares a high-half multiply against a divide, which is the
comparison the magic-number rewrite turns on.

## Proposed fix

1. **Add a power-of-two signed remainder rule to GlobalISel** —
   `srem_pow2` beside `urem_pow2_to_mask`, building
   `X - ((X sdiv 2^k) << k)` from the `sdiv_by_pow2` expansion.
   Order it ahead of `srem_by_const` so a power of two never reaches
   the magic-number builder.  With the hook false this is a strict
   improvement (the first table) and makes the pipelines agree.
2. **Make the two pipelines gate power-of-two lowering the same
   way.**  Today SelectionDAG consults `isIntDivCheap()` for the
   power-of-two divide and remainder and GlobalISel does not.  The
   rule from item 1 should follow whichever convention is chosen.
3. **Stop asking one question about two kinds of sequence.**  The
   power-of-two sequences contain no multiply, so their gate
   plausibly belongs on `MinSize` alone; the magic-number rewrite
   needs a predicate about multiply-versus-divide cost.  For the
   targets that answer the hook only under `MinSize` the change is
   behaviourally invisible; it matters for targets that answer it
   for speed, and for WebAssembly, which answers it unconditionally.

Items 1 and 2 are self-contained and testable with
`-run-pass=legalizer` and combiner MIR tests plus end-to-end `llc`
runs of the IR above on both pipelines.  Item 3 touches the hook's
contract and would want its own discussion.

A related gap, separate from this one: GlobalISel's lack of any
counterpart to `decomposeMulByConstant` and `DAGCombiner::visitMUL`'s
decompositions, noted in the previous section.

## Status in this tree

Penumbra's `isIntDivCheap()` gives a speed answer, as the contract
allows: `MUL` and `DIV` share one sequential `divmul` unit, and the
hook compares the tuned core's divide latency against two multiplies
plus fixups, so it is true on a core that multiplies as slowly as it
divides and false where the multiply is much faster.

Penumbra carries the item 1 rule locally as `penumbra_srem_pow2` in
its pre-legalizer combiner, so a signed `x % 2^k` lowers without a
divide at `-O1` and above on every core.  The `-O0` combiner has no
divide rules; there only the unsigned power-of-two cases reduce,
through Penumbra's own legalizer.  The surrounding work is tracked in
[the constant strength-reduction entry](./TODO.md#compiler-constant-multiply-divide-and-remainder-strength-reduction).

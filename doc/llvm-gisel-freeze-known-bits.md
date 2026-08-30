# LLVM GlobalISel known-bits cannot see through `G_FREEZE`, and the combiner plants one on every `||`

## TL;DR

`GISelValueTracking::computeKnownBitsImpl`
(`llvm/lib/CodeGen/GlobalISel/GISelValueTracking.cpp`) has no
`G_FREEZE` case.  A frozen value falls through to the default arm and
nothing is known about it.

IR's `computeKnownBits` does handle this
(`llvm/lib/Analysis/ValueTracking.cpp`, `Instruction::Freeze`): freeze
of a well-defined value *is* that value, so its known bits carry
through.  The recursion is guarded by `isGuaranteedNotToBePoison`,
because freeze of poison picks an arbitrary bit pattern instead.

This matters more in GlobalISel than the gap alone suggests, because
GlobalISel **plants the freezes itself**.  `matchSelectToLogical`
(`llvm/lib/CodeGen/GlobalISel/CombinerHelper.cpp`, reached from
`select_combines` inside `all_combines`) rewrites

```
select Cond, 1, F   ->   or  (zext Cond), (freeze F)
select Cond, T, 0   ->   and (zext Cond), (freeze T)
```

which is the shape every short-circuit `||` and `&&` arrives in once
SimplifyCFG has speculated the right-hand side.  The pre-legalizer
combiner therefore introduces a known-bits barrier that the
post-legalizer combiner cannot see past, and `redundant_and` stops
firing on booleans that flow through a logical operator.

## Reproducer

Upstream target, no Penumbra involved:

```c
extern int g(void);
extern int flag;
extern void sink(void);

void f(void) { if (g() != 0 || flag) sink(); }
```

```sh
clang --target=riscv32-unknown-elf -O2 -S -emit-llvm -o f.ll f.c
llc -mtriple=riscv32 -global-isel -O2 f.ll -o -
```

The `||` survives InstCombine as a select, because the call on the
left is not speculatable:

```llvm
%2 = icmp ne i32 %1, 0
%4 = icmp ne i32 %3, 0
%5 = select i1 %2, i1 true, i1 %4
br i1 %5, ...
```

After the pre-legalizer combiner:

```
%2:_(s1) = G_ICMP intpred(ne), %0(s32), %1
%5:_(s1) = G_ICMP intpred(ne), %3(s32), %1
%9:_(s1) = G_FREEZE %5
%6:_(s1) = G_OR %2, %9
```

and the selected code keeps a mask that cannot be justified from
anything the machine does:

```asm
snez  a0, a0
snez  a1, a1
or    a0, a0, a1
xori  a0, a0, 1
andi  a0, a0, 1     # redundant: both snez results are 0/1
bnez  a0, .LBB0_2
```

RISC-V declares `ZeroOrOneBooleanContent`, so bits 31:1 of each `snez`
result are known zero and the `andi` should fold away.  It does not,
because the `G_FREEZE` between the second `G_ICMP` and the `G_OR`
erases the fact.  SelectionDAG on the same input emits neither the
mask nor the `xori`.

## Why the obvious port of the IR case is inert

Adding the `Instruction::Freeze` logic to `computeKnownBitsImpl`
verbatim — recurse into the source when `isGuaranteedNotToBePoison`
holds — changes nothing measurable, because the MIR poison analysis is
much weaker than the IR one.

`isGuaranteedNotToBeUndefOrPoison` (GlobalISel's `Utils.cpp`) bottoms
out in `canCreateUndefOrPoison`, whose default arm is

```cpp
return !isa<GCastOp>(RegDef) && !isa<GBinOp>(RegDef);
```

so every opcode that is neither a cast nor a binary operator counts as
poison-creating.  `COPY` — how function arguments and call return
values enter a function — and `G_LOAD` both land in that arm.  In the
reproducer above the two comparison operands are exactly a call return
(`COPY`) and a load, so the guard fails and the recursion never runs.

The guard is genuinely required: freeze of poison may yield any bit
pattern, so without it the known bits are unsound.  The fix has to
come from the poison side.

## Proposed fix

Two patches, in this order:

1. **`G_FREEZE` in `computeKnownBitsImpl`.**  Mirrors the existing IR
   code and is a few lines:

   ```cpp
   case TargetOpcode::G_FREEZE: {
     Register Src = MI.getOperand(1).getReg();
     if (isGuaranteedNotToBePoison(Src, MRI, Depth + 1))
       computeKnownBitsImpl(Src, Known, DemandedElts, Depth + 1);
     break;
   }
   ```

   `computeNumSignBits` has the same hole and wants the same
   treatment.

2. **Strengthen the MIR poison analysis** enough for (1) to fire.  At
   minimum `G_LOAD` and `COPY` from a non-poison source should stop
   being treated as poison-creating; IR's `canCreateUndefOrPoison`
   enumerates opcodes rather than falling back on a structural test,
   and moving MIR's toward that shape is the honest fix.

Patch (1) has no test that shows a win on its own, which is worth
saying out loud in the review — its justification is that it is the
correct known-bits rule and the prerequisite for (2).  A combined
test can use the reproducer above and check that the `andi` is gone.

## Status in this tree

Not carried.  `GISelValueTracking.cpp` is unmodified; both patches
were prototyped locally and reverted, and the numbers below come from
those prototypes.

The cost showed up when Penumbra moved from s1 booleans to
register-width booleans with `setBooleanContents(ZeroOrOneBooleanContent)`.
Under the previous `UndefinedBooleanContent` the hole was invisible:
known-bits learned nothing from a comparison in the first place, so
there was no fact for the freeze to block.

Measured on `sw/doom/chocolate-doom/src/doom/p_saveg.c`, whose
save-game writers are dense in `||`, counting `and rX, 1`:

| configuration | masks |
|---|---:|
| s1 booleans, `UndefinedBooleanContent` | 158 |
| register-width booleans, `ZeroOrOne` | 238 |
| … plus patch (1) alone | 238 |
| … with the poison guard removed (unsound; diagnostic only) | 159 |

So the freeze hole accounts for the entire regression, and patch (2)
is what actually recovers it.

Penumbra does not wait for it.  The surviving `G_AND %bool, 1` selects
to an `ANDi` that sets Z from the masked value, and the `TESTi` it
feeds then recomputes the same Z — a redundancy between two machine
instructions, which the compare-elimination peephole erases
(`producesNZFlagsForMask` in `PenumbraInstrInfo.cpp`).  That needs no
analysis to prove anything, so it also reaches the sites a fixed
`G_FREEZE` rule could not: an incoming `i1` argument carries an ABI
fact about its register, not a known-bits fact.  The upstream patches
remain worth landing on their own merit — RISC-V pays for the hole
too, as the reproducer above shows.

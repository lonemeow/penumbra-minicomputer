# PeepholeOptimizer merges constant-physreg reads into one spilled live range (worse than -O0)

Upstream LLVM issue writeup. Self-contained; the discovery vehicle was a
downstream target (Penumbra), but the responsible pass —
`PeepholeOptimizer` — and every routine named below are target-independent,
so this reproduces on any target that reads a *reserved, never-defined*
physical register (a thread pointer / per-CPU base) at several points
separated by calls within one basic block. RISC-V's `tp` (carrying
`curlwp`/`curcpu` in the NetBSD and Linux kernels) is the canonical real
example.

## Summary

When a function reads a **constant physical register** (reserved and never
redefined — e.g. `tp`/`x4`) at multiple points within a basic block, and
those points are separated by calls, `PeepholeOptimizer::foldRedundantCopy`
rewrites every read to reuse the **first** read's virtual register, fusing N
short, non-overlapping live ranges into one range that spans all the calls.

That single range:
- cannot stay in the source register (it is reserved / non-allocatable), and
- is **not** rematerialized by the coalescer or spiller, because a reserved
  register has no tracked `LiveInterval`, so neither can prove the source is
  available at a re-read point.

So the allocator parks the value in a callee-saved register (forcing a
prologue save/restore) or stack-spills it. The result is **strictly worse
than `-O0`**, which never runs `PeepholeOptimizer` and emits one free
register-to-register move per use.

This is a missed optimization, not a miscompile: `getCopySrc` only admits a
physreg source when `MRI->isConstantPhysReg()` holds, so the value is
genuinely invariant and reusing it is semantically correct — just not
profitable here.

## Reproducer

The trigger is `llvm.read_register` of a reserved register used repeatedly
across calls. On RISC-V (`tp` = `x4` is reserved by default):

```llvm
; llc -mtriple=riscv64 < repro.ll              # -O2: spills; -O0: clean
; llc -mtriple=riscv64 -O0 < repro.ll
; llc -mtriple=riscv64 -disable-peephole < repro.ll   # -O2 but clean
declare void @g(i64)
declare i64 @llvm.read_register.i64(metadata)

define void @three_uses() {
  %a = call i64 @llvm.read_register.i64(metadata !0)
  call void @g(i64 %a)
  %b = call i64 @llvm.read_register.i64(metadata !0)
  call void @g(i64 %b)
  %c = call i64 @llvm.read_register.i64(metadata !0)
  call void @g(i64 %c)
  ret void
}
!0 = !{!"x4\00"}
```

Expected (`-O2`, today): one `mv <callee-saved>, tp` plus a spill/reload of
that callee-saved register around the calls.
Wanted (matches `-O0` / `-disable-peephole`): `mv a0, tp` immediately before
each call, no callee-saved register used, no spill.

> NOTE: the trace below was captured on a downstream target and verified
> end to end. The RISC-V form above is the same pattern on an upstream
> target and is expected to reproduce because `PeepholeOptimizer` is
> target-independent — confirm on an upstream build when filing.

## Verified pass-level trace

Reading the register three times around calls. After ISel / Legalizer there
are three independent, non-overlapping reads (none crosses a call):

```
%0 = COPY $tp ; $a0 = COPY %0 ; CALL @g ; (… %0 dead …)
%2 = COPY $tp ; $a0 = COPY %2 ; CALL @g
%4 = COPY $tp ; $a0 = COPY %4 ; CALL @g
```

After **`PeepholeOptimizer`** (3 reads → 1):

```
%0 = COPY $tp
$a0 = COPY %0 ; CALL @g
$a0 = COPY %0 ; CALL @g      ; %0 now lives across every call
$a0 = COPY %0 ; CALL @g
```

`-print-after-all` shows the count of `COPY $<reg>` drop from 3 to 1 exactly
at "Peephole Optimizations"; it is unchanged by `-disable-machine-cse` and
the GlobalISel CSE flags, and `-disable-peephole` (or `-O0`) restores the
optimal code. So the responsible pass is unambiguously `PeepholeOptimizer`,
not GVN/EarlyCSE (which `llc` does not run), MachineCSE, or the register
coalescer (the merge is already present in the coalescer's input).

## Root cause (file/function pointers)

`llvm/lib/CodeGen/PeepholeOptimizer.cpp`:

- `getCopySrc(MI, SrcPair)` admits a COPY's source for redundant-copy
  tracking when the source is virtual **or** `MRI->isConstantPhysReg(SrcReg)`
  is true. The `isConstantPhysReg` arm is what lets a reserved register
  enter `foldRedundantCopy`.
- `foldRedundantCopy(MI)` keeps a per-source map `CopySrcMIs`. The first
  `COPY $tp` is recorded; each subsequent one hits
  `MRI->replaceRegWith(DstReg, PrevDstReg)` and clears kill flags, with the
  comment *"Lifetime of the previous copy has been extended."* That extension
  across the calls is the regression.
- `CopySrcMIs.clear()` runs per basic block, not on register clobbers. For a
  *constant* physreg that is correct (calls can't change it), which is
  precisely why reads separated by calls are merged.

`llvm/lib/CodeGen/MachineRegisterInfo.cpp`:
- `isConstantPhysReg(tp)` is true (reserved, not allocatable, no defs), so
  the COPY's source qualifies.

Why the damage isn't undone later:
- `RegisterCoalescer` (`joinReservedPhysReg`) and the spiller can rewrite a
  vreg back to / rematerialize from a physreg only via tracked liveness;
  reserved registers have no `LiveInterval`, so the merged value is treated
  as an ordinary cross-call value — assigned a callee-saved register or
  stack-slot, never re-read from the source.

## Why `-O1+` is worse than `-O0`

`PeepholeOptimizer` runs at `-O1+` and not at `-O0`. Its redundant-copy
heuristic assumes that reusing one copy is cheaper than recomputing the
value. That holds when recomputation is expensive. For a constant physreg the
"recomputation" is a single register move that is *free at every program
point*, so fusing the reads only adds register pressure — and when the fused
range crosses a call, it forces a spill that the individual reads never
needed.

## Fix directions

1. **Make `foldRedundantCopy` cost-aware for constant-physreg sources**
   (most targeted): when `SrcReg` is a physreg, do not extend the live range
   across a call / clobber boundary, or more generally skip the fold when the
   merge would lengthen the range past a point where the cheap re-read would
   otherwise have sufficed. Re-reading a constant physreg is a single COPY, so
   the fold has essentially no upside for physreg sources and a clear
   downside.
2. **Teach the coalescer/spiller to rematerialize a COPY from a constant
   physreg** (broader): give constant physregs an "available everywhere"
   notion so `reMaterializeTrivialDef` / the spiller re-issue
   `vreg = COPY $physreg` at the use instead of extending/spilling. This also
   fixes the symmetric coalescer-side and allocator-side variants, but is more
   invasive.
3. **Drop the `isConstantPhysReg` arm in `getCopySrc`** (simplest, bluntest):
   stop tracking physreg sources for redundant-copy folding. Risks regressing
   whatever motivated adding it; bisecting that commit would show whether any
   in-tree test depends on it.

Option 1 is the smallest change that removes the regression without losing
the virtual-register redundant-copy folding that is the pass's main job.

## Impact

Every target that pins a per-thread / per-CPU pointer in a reserved GPR and
reads it repeatedly around calls in hot code (locks, scheduler, fault paths)
pays this: the reads that should be free moves become callee-saved spills.
RISC-V `tp`-as-`curlwp` is the prominent case; SPARC `%g7`, PowerPC `r13`,
and any `-ffixed-rN` global-register variable are equally affected. Targets
using a segment base (x86 `%gs`) or a system register (AArch64 `TPIDR_*`)
sidestep it because the access never becomes a long-lived GPR value.

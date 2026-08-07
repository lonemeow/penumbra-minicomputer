# Penumbra exposure to the known upstream GlobalISel miscompiles (audited 2026-08-02, LLVM 22.1.2 tree)

## TL;DR

Audited against the known upstream GISel correctness-bug families
(llvm-project #210470/#203972 combiner flag transplant, #204182
widenScalar flag retention, #199386 lowerBswap, the trunc-of-shift
amount-type skew fixed by #213381, #177416 artifact-combiner ICE).

**No runtime miscompile is live today.** But both wrap-flag corruption
mechanisms are demonstrably ACTIVE in our pipeline — they deposit false
`nuw` flags into gMIR on every build — and are harmless only because
LLVM 22.1.2 contains (almost) no code that *reads* those flags. Trunk
does. **An LLVM rebase converts these from latent to live miscompiles
with zero Penumbra-side changes**, and we have no SDAG fallback to hide
behind. Treat the flag-hygiene fixes as a rebase prerequisite.

Penumbra sits in exactly the affected class upstream triage identified:
the transplanting rule sets are reachable only for "explicit GlobalISel
at -O1+" configurations — which describes every Penumbra build, always.

## Mechanism 1: combiner pattern-rule flag transplant (#210470 / #203972)

`propagateFlags` in `GIMatchTableExecutorImpl.h` (:76, applied at the
GIR_*Done actions) copies ALL of the matched root's MIFlags onto every
instruction a TableGen *pattern* apply creates. When the rewrite changes
the operation, poison flags (`nuw`/`nsw`/`exact`/`disjoint`) become
unfounded claims. C++ `build_fn` applies are immune (they build flagless
or set flags deliberately).

**Verified live in our pipeline** (llc 22.1.2, `-stop-before=legalizer`):

```llvm
define i32 @mulneg(i32 %x) {            ; mul_by_neg_one ∈ trivial_combines
  %r = mul nuw i32 %x, -1               ; fires at -O0 AND -O2
  ret i32 %r
}
;  =>  %2:_(s32) = nuw G_SUB %3, %0     ; transplanted nuw claims x == 0
```

Rule inventory in our tree: `integer_reassoc_combines` (Combine.td:1913)
contains 4 of the 8 upstream runtime-verified miscompiling rules
(`AMinusBPlusCMinusA`, `AMinusBPlusBMinusC`, `APlusBMinusAplusC`,
`APlusBMinusCPlusA`); the upstream flagship (`APlusBMinusCMinusB`) and
`add_shift` postdate 22.1.2 and arrive on rebase.

**Why it's latent here:** 22.1.2's `GISelValueTracking.cpp:297` computes
G_SUB known-bits as `KnownBits::sub(Known, Known2)` — flag-blind. The
only wrap-flag readers in the whole 22.1.2 GISel library are
`matchSextOfTrunc` (G_TRUNC nsw, CombinerHelperCasts.cpp:40) and
`matchAddOverflow` (G_ADD nuw/nsw under G_U/SADDO, CombinerHelper.cpp:7991)
— and a census of Combine.td shows **zero pattern rules that create
G_ADD or G_TRUNC**, so no transplant can feed either consumer.
`sub_to_add`, the obvious feeder for the addo fold, is already correct
in 22.1.2 (`matchCombineSubToAdd` explicitly does
`clearFlag(NoUWrap)`, CombinerHelper.cpp:2157).

Verified empirically: upstream's flagship witness shape (with/without
`sub nuw`, expecting the 255-mask to survive) produces byte-identical,
correctly masked asm for both variants at -O2.

**Why trunk detonates:** current trunk's `GISelValueTracking` G_SUB is
flag-aware (`KnownBits::sub(..., NoUWrap, NoSWrap)`), and
`redundant_and` / `redundant_or` — both in `known_bits_simplifications`,
which we run in the **post-legalizer** combiner — fold on the bogus
knowledge. Upstream verified 8 runtime miscompiles this way (e.g.
`f(1,2,7)` = 250 correct vs -6 miscompiled). Fix direction upstream
(#210470, nikic-endorsed): drop poison flags by default in combiner
inheritance. Not landed as of 2026-08-02.

## Mechanism 2: widenScalar keeps `nuw` across anyext widening (#204182)

`LegalizerHelper` widens ops in place (`setDesc`/operand rewrite) and
never drops flags (`dropPoisonGeneratingFlags` does not appear in the
22.1.2 file). A sub-word wrap flag — honest at i16 — survives onto the
s32 op whose operands are `G_ANYEXT` garbage, where the claim is
unfounded.

**Verified live in our pipeline** (`-stop-after=legalizer`):

```llvm
define i16 @widen_nuw(i16 %a, i16 %b) {
  %r = sub nuw i16 %a, %b
  ret i16 %r
}
;  =>  %8:_(s32) = nuw G_SUB %2, %3     ; %2,%3 = G_ANYEXT of the i16s
```

Penumbra is *more* exposed to this than RV64 was upstream: we widen
every sub-word ALU op to s32, and our post-legalizer combiner runs
`known_bits_simplifications` directly over the widened output. Latent
for the same single reason as mechanism 1 (flag-blind 22.1.2 KB);
detonates on the same rebase.

## The rest of the known families

- **#199386 `lowerBswap` sign-extended mask (s64+ miscompile):** our
  vendored `LegalizerHelper.cpp` still contains the buggy
  `0xFF << (i * 8)` construction — but we are immune because we narrow
  s64 G_BSWAP to two s32 halves before lowering, precisely to dodge
  this (independently root-caused in `doc/llvm-lowerBswap-bug.md`;
  upstream fixed it in #199387 with `APInt::getBitsSet`). Backport
  #199387 on the next tree touch so the narrowing order stops being
  load-bearing correctness.
- **trunc-of-shift amount-type skew** (`applyCombineTruncOfShift`
  keeps the stale wider amount type; fixed upstream in #213381,
  post-22.1.2): present in our tree, but probed benign —
  `trunc(lshr i64, 8) → i32` emits a correct shr/shl/or pair; on a
  32-bit target the legalizer re-narrows the amount and the semantics
  survive. Worst case is ICE-class, not silent. Fix arrives free on
  rebase.
- **#177416 merge/unmerge artifact-combiner regroup:** ICE
  (`UnableToLegalize`), not a miscompile. Pure-GISel Penumbra fails
  loudly if hit; our s64-heavy narrowing exercises merge/unmerge a lot
  and the compiler-correctness suite runs clean.

## Penumbra-side hygiene (checked clean)

- `applyNegImmToOpposite` (our G_ADD↔G_SUB constant flip — the exact
  shape that needed `clearFlag` upstream) builds **flagless**: correct
  by conservatism. Same for `applySinkPtrAddPastUse` (`buildPtrAdd`
  drops the original's flags) and `applyZextloadPromote`.
- No Penumbra code reads wrap flags (selector/legalizer/combiner
  greps clean), so we cannot self-detonate today.

## Action items

1. **Rebase gate:** before (or as part of) any LLVM upgrade, confirm
   the combiner poison-flag drop (#210470 fix) is in the tree — or
   carry it as a local patch. Without it, the upgrade imports the
   full 8-rule miscompile family into a compiler with no fallback,
   plus flag-aware KnownBits to detonate our own widenScalar residue.
2. **Same gate applies to local work:** adding flag-aware KnownBits to
   our tree (e.g. for codegen quality) would detonate mechanisms 1+2
   immediately. Don't, until the hygiene fixes are in.
3. Backport #199387 (`lowerBswap` mask fix) opportunistically.
4. Re-run this audit's probes after any rebase:
   the three probe functions above plus the flagship mask-survival
   pair; all five live in this doc and take one llc invocation.

# Compiler: selection — compares, flags, extensions

## Compiler: zero words of split 64-bit values are not folded

The legalizer splits 64-bit and wider values into 32-bit words.  A
word that is zero should let its consumers fold, but two cases escape
the combiner.

**A zero behind a split PHI.**  When a 64-bit value is merged at a join
and one of its words is the same constant on every edge, splitting the
PHI leaves that word as `phi(0, 0)`.  The combiner's rules match
`G_CONSTANT`, so none of them fires.  Two ways in:
- `extend_through_phis` in the pre-legalizer combiner moves a
  zero-extension above a join into each predecessor, turning the high
  word into a PHI of zeros.  The kernel's `disk_set_info` loads its
  geometry fields on two paths before
  `(int64_t) dg_nsectors * dg_ntracks * dg_ncylinders`.
- A 64-bit select or PHI whose arms share a zero word: SoftFloat's
  `z = ... ? 0xFFFFFFFF00000000 : (a0 / b0) << 32` in `__divdf3`.

What it costs depends on whether a constant zero would have folded.
Against the same code with `extend_through_phis` disabled, on a value
zero-extended through a join: a multiply keeps its zero partial
products, three multiplies and an add where one `MULU_P` would do (37
cycles a multiply on gen1, 10 on gen2 and gen2.5); an `and` computes
`0 & y`; a shift shifts the zero word.  Add, subtract, compares, `or`
and `xor` need the zero in a register either way and cost the same.

**A high half by a constant zero.**  The post-legalizer combiner folds
`G_MUL x, 0` (upstream's `binop_right_to_zero`), but nothing folds
`G_UMULH x, 0`, which selects to a real `MULU_P`; SelectionDAG does
fold `mulhu x, 0`.  Multiplies wasted per shape:
- 64-bit `x * 0x3_0000_0000`, a constant with a zero low word: 1;
- unsigned 64-bit `__builtin_mul_overflow` with one operand widened
  from 32 bits: 2;
- `(unsigned __int128)a * b` of 64-bit values, the full product or its
  high half (`i128-mulhi.ll`'s `umulh64`): 2; the high half with `b`
  32 bits wide: 3.

A plain 64-bit multiply is unaffected even with zero-extended operands
(the zero words meet only in low halves), as are signed forms (a
sign-extended word is not a constant) and a 32x32->64 widening
multiply.

Fix, once and generically rather than per operation: a post-legalizer
rule folding a `G_PHI` whose incoming values are all one constant into
that `G_CONSTANT`, after which every existing constant rule fires —
the multiply collapses to one `MULU_P` through `binop_right_to_zero`
and the `G_MUL` + `G_UMULH` fusion, with no change to the widening
detection; and `binop_right_to_zero` extended to `G_UMULH`/`G_SMULH`,
which the PHI fold also needs where a high-half product meets the zero
word (`__divdf3`).  Both belong upstream in `Combine.td`, where they
serve every 32-bit GlobalISel target.  The PHI fold has to run after
legalization: when the PHI is split, its incoming values are still
64-bit loads, and the zero words appear only once those are narrowed.
Keeping `extend_through_phis` from widening a PHI into an illegal type
would close one way in but not the shared-word one.

Reach, counting multiplies whose zero is visible in the same block (a
lower bound): the `GENERIC.DEBUG` kernel 3 (`disk_set_info` 2,
`diskerr` 1), `libc.so` 2 (`__divdf3`), Doom none, the
`test-compiler` corpus 2 (the `mulhi64` test, the high-half case).

## Compiler: compare-elim splice past ADC/SBC — RESOLVED; frame-index hazard open

The compare-elimination peephole's backward walk skipped SR readers,
so its splice could move a carry-producing `ADD`/`SUB` past the
`ADC`/`SBC` consuming its carry, breaking i64 arithmetic.  Fixed:
the walk bails on any gap SR reader; the repro is a negative case in
`test/CodeGen/Penumbra/compare-elim.ll`.  Binaries built between the
peephole extension and the fix are suspect until rebuilt — that
window covers every installed userland, so the fix reaches the
board only with a distribution rebuild.

Still open from the same audit
([`doc/llvm/flag-reuse-notes.md`](../llvm/flag-reuse-notes.md)): the
`eliminateFrameIndex` large-offset path emits a post-RA `ADD` that
clobbers SR inside a live-flags window for frames over 32 KiB.
Flags cannot be saved in software (`WRSPR SR` traps), so the fix is
relocation — check SR liveness at the insertion point and emit the
materialisation above the nearest earlier SR def.

## Compiler: compare-elim does not look through COPY

`optimizeCompareInstr`'s backward walk takes the first instruction that
defines the compared register as the flag producer, and gives up unless
it is flag-setting.  A `COPY` in between stops it —
`definesFlagsFromResult(COPY)` is false — even though a `COPY` neither
touches SR nor changes the value.

The shape reaching it today comes from `selectZExt`, which emits a
`COPY` rather than a mask when zero-extending a carry-out that
`emitCarryToValue` already made 0/1:

```
%10:gpr = ANDi %27, 1, implicit-def $sr   ; sets Z from the result
%19:gpr_allocatable = COPY %10
TESTi %19, 1, implicit-def $sr             ; recomputes the same Z
```

Three sites in a 120-file Doom build, more in i64-heavy code.  The
`SUB`-then-`COPY`-then-`CMP` form named in the peephole's own header
comment is the same gap.

Following the `COPY` means retargeting the walk to its source, and that
collides with the splice: the `COPY` then *reads* the producer's def,
so splicing the producer down past it would leave the `COPY` reading an
undefined register.  The way out is that the splice is keyed on
adjacency when what it exists for is making the producer the most
recent SR write.  Keyed instead on "some instruction in the gap
modifies SR", the two compose — a `COPY`-only gap needs no splice and
its reads are harmless, while a gap that does clobber SR still splices
and the existing "gap must not read the compared register" check still
rejects the unsafe case.

Treat as delicate: this is the walk that produced the ADC/SBC splice
miscompile above.  The negative cases belong in
`test/CodeGen/Penumbra/compare-elim-mask.mir` beside the mask ones.

The same re-keying covers a gap that is not a `COPY`:
`sub r, x; stw r, [...]; cmp r, 0`, where the store reads the compared
register without touching SR.  106 kernel sites, including
`KPREEMPT_ENABLE` on every `userret`.

Separate and not covered by the walk: 164 kernel sites repeat an
identical compare at the head of a fall-through block whose
predecessor already set the same flags.  That redundancy crosses a
block boundary, where a backward walk cannot see it.  SQLite's
repeats (161 same-block, 407 across labels) come mostly from the i64
compare expansion in "64-bit relational compares expand to select
diamonds", since resolved; recount before acting on them.

## Compiler: s1 booleans lose their comparison through logic ops — RESOLVED

Booleans are register-width now: `G_ICMP`/`G_FCMP` produce s32 and
`G_BRCOND`/`G_SELECT` consume it, matching AArch64 and RISC-V, with
`setBooleanContents(ZeroOrOneBooleanContent)` declaring the 0/1
guarantee.  The carry linking `G_UADDO` to `G_UADDE` stays s1: it is
the SR.C bit rather than a value, and `narrowScalarAddSub` creates
those carries as s1 unconditionally, so any other width would only be
widened straight back apart.  −1.17% on a 120-file Doom build.

The original diagnosis here was wrong and is worth recording, because
it sent the first attempt at the wrong target.  The ext/trunc pair
around a logic op costs nothing — `G_ANYEXT`/`G_TRUNC` select to
`COPY` and coalesce away, and `(a>=b)|(c>=d)` always selected to
`cmp`/`adc` pairs plus an `or`.  Two other things were doing the
damage:

- **Signed compare-to-value.**  InstCombine rewrites `x >> 31` into
  `icmp slt x, 0` whenever it feeds an `i1` operator, and
  `selectICmpToValue` bails on every signed predicate.  Independent of
  boolean width; still open, see the entry below.
- **`BitWidth > 1` in `GISelValueTracking`.**  The known-bits rule for
  a comparison result is gated on the result being wider than one bit,
  so `ZeroOrOneBooleanContent` was inert while `G_ICMP` produced s1.
  That is why the earlier `setBooleanContents` experiment measured no
  benefit — the two changes only work together.

`lowerThreewayCompare` picks the arithmetic form over the select chain
once boolean contents is declared, which regressed `scmp_i32`; the
`preferSelectsOverBooleanArithmetic` hook holds it on the select
chain.

Widening leaves `G_AND %bool, 1` behind, which `redundant_and` cannot
drop because a `G_FREEZE` blocks known-bits — an upstream hole written
up in
[`doc/llvm/gisel-freeze-known-bits.md`](../llvm/gisel-freeze-known-bits.md).
Rather than wait for it, the compare-elimination peephole erases the
`TESTi` those masks feed, since the mask already set the same Z.

## Compiler: signed comparisons have no branchless materialisation

`selectICmpToValue` resolves unsigned and eq/ne predicates to one
compare plus a carry read.  Signed predicates have no flag that answers
them, so they fall through to the `SELECT_CC` diamond — `lli 1` /
`lli 0` / `cmp` / `Bcc` / `mov`, six instructions and a data-dependent
branch.  `emitSelectCC` shares the diamond, so `G_SELECT` on a signed
predicate pays the same.

The sign-bit tests are done: `slt x, 0` is `SHR #31` and `sge x, 0` is
`NOT` then `SHR #31`, as two patterns in `PenumbraGISel.td`.  They
matter beyond their site count because InstCombine rewrites `x >> 31`
into `icmp slt x, 0` whenever the value feeds an `i1` operator, so
source written as a shift still arrives as a comparison.

What is left has no cheap closed form:

- `sgt x, 0` and `sle x, 0` compare against zero but test more than the
  sign bit.  `((0 - x) & ~x) >> 31` is five instructions — branchless,
  but no smaller than the diamond.  Worth it only where the branch is
  hot enough to pay for itself.
- Two variable operands have nothing.  Biasing to unsigned
  (`(a ^ 0x80000000) <u (b ^ 0x80000000)`) costs more than the diamond
  once the constant is materialised.

Remaining sites, counted on binaries built before the sign-bit
patterns landed:

| binary | diamonds | of which sign-bit tests |
|--------|---------:|------------------------:|
| NetBSD `GENERIC.DEBUG` | 565 | 139 |
| `libc.a`               | 408 |  83 |
| Doom, 120 files        | 117 |  11 |

The kernel's are scattered across calendar arithmetic and filesystem
block mapping (`clock_secs_to_ymdhms`, `ufs_bmaparray`, `ffs_balloc`),
not in syscall entry, trap handling or `copyin`/`copyout` — so expect
code size rather than a benchmark to move.

## Compiler: compare and branch selection misses immediate and TEST forms

`emitCompare` folds an immediate only when the constant fits uimm16 as
given, and `selectBrCond` either consumes a compare or tests a
materialized boolean.  Four shapes pay for that (static counts,
2026-09-13):

| Source | Emitted | Wanted | Sites |
|---|---|---|---|
| `x >= 0` (`icmp sgt x, -1`) — DONE | `llis t,-1; cmp x,t; ble` | `cmp x,0; blt` | kernel 274, SQLite 612, Doom 86 |
| `(x & C) != 0` — DONE | `mov t,x; and t,C; beq` | `test x,C; beq` | Doom 140 |
| branch on a non-compare `i1` whose true edge falls through — DONE | `lli t,1; xor t,b; test t,1; bne` | `test b,1; beq` | kernel 848 |
| `x == -1` | `llis t,-1; cmp x,t; beq` | `not t,x; beq` | Doom 53 |

- **Immediate adjustment — DONE.**  `sgt x, -1` is `sge x, 0`.
  `canonicalizeCompare` moves the predicate and the constant by one
  when only the adjacent constant fits the immediate (`sgt C` ↔
  `sge C+1`, `slt C` ↔ `sle C−1`, and the unsigned pairs), for
  branches and selects alike, and refuses a step that would wrap in
  the predicate's signedness.  Shape coverage: `compare-imm-test.ll`,
  `GlobalISel/select-compare-immediate.mir`.  The `llis -1; cmp` pairs
  left (kernel 356, `libsqlite3` 25, Doom 51) are `x == -1` and the
  byte and halfword sign tests in "signed sub-word loads in relational
  compares miss LDBS/LDHS".
- **Branch on N for sign tests.**  After `cmp x, 0` V is clear, so
  `bmi`/`bpl` are exact for `x < 0`/`x >= 0`.  Reading N alone also
  lets compare elimination drop the compare after a producer whose V
  means something else — the `XOR` in Doom's `R_PointOnSide`, the
  `SUB` in `R_AddLine` and `R_CheckBBox`.
- **TEST fold — DONE.**  The `mov` existed because `AND` is
  destructive and `x` stays live.  A branch on `(x & m) == 0` or
  `!= 0` whose AND has no other user now selects `TESTi x, m`, or
  `TEST x, m` for a wider mask.  Selects keep the AND.  Copy-and-mask
  tests went from 1,243 to 164 in the kernel, 706 to 35 in
  `libsqlite3` and 130 to 15 in Doom.

  Together with the immediate adjustment, measured statically on the
  hot-path review corpus (2026-09-13): kernel −1,877 instructions
  (`.text` −7,508 B, 18 fewer callee-saved saves), `libsqlite3`
  −1,461, Doom −253.  `trap` is 12 instructions shorter.  31 kernel
  functions grew, the largest `do_sys_mount` by 20 of 556, all in
  spills and reloads.
- **Inverted boolean — DONE.**  The branch-inversion combine wraps
  the condition in `G_XOR %v, 1` when the true edge is the layout
  successor.  A compare absorbed it into the predicate, but any other
  `i1` — a call result, a `bool` load, a phi — reached `selectBrCond`
  with the XOR still in place, and selection materialized and tested
  it.  `selectBrCond` now looks through single-use `XOR c, 1` and
  `AND c, 1` wrappers and flips the branch sense.  The corpus's
  `lli 1; xor` pairs dropped from 1,071 to 32 in the kernel, 199 to
  42 in `libsqlite3` and 88 to 18 in Doom; those left feed values,
  not branches.  Shape coverage: `brcond-inverted-bool.ll`.  RISC-V's
  GlobalISel output has the same `xori`, so this is upstream-worthy.

## Compiler: share the hi-word compare in i64 three-way compares — RESOLVED

The `G_SCMP`/`G_UCMP` lowering (`.lower()` →
`LegalizerHelper::lowerThreewayCompare()`) expands the i64 case
verbosely: 16 BBs — two s64 ICMPs each become a three-block hi/lo/eq
diamond, materialized as four `mov` selects.  A peephole that shares
the hi-word compare across the two ICMPs would shrink it.  Not on any
hot path (qsort's comparator is the i32 case); low priority.  The
two-way relational compares have the same problem and are hot — see
the next entry.

Resolved with the next entry: each of the two s64 compares is now one
`CMP` + `CMPC` chain, so no high-word compare repeats, and
`scmp`/`ucmp` of i64 take 16 instructions where they took 35
(`threeway-cmp.ll`).

## Compiler: 64-bit relational compares expand to select diamonds — RESOLVED

`LegalizerHelper::narrowScalar` splits an s64 `G_ICMP` into
`select(hi == hi', ult(lo, lo'), pred(hi, hi'))`.  Selection builds
the signed half with a branch diamond (`selectICmpToValue` has no
signed path), the `G_SELECT` with a second one, and emits the
high-word compare twice.  A branch on signed `a < b` takes 12
instructions on its path:

```
cmp r1,r3; mov r1,zero; adc r1,zero; lli r3,1; lli r11,0
cmp r2,r4; blt ...; cmp r2,r4; bne ...; test r1,1; bne ...
```

A borrow chain does it in four: `cmp alo,blo; mov t,ahi; sbc t,bhi;
blt` (`blo` unsigned).  `MOV` leaves the flags alone, so the copy can
go anywhere before the `SBC`.  Only `lt`/`ge` come out of the chain
directly — Z reflects just the high word — so `gt`/`le` swap operands.

| Source | Now | riscv32 SelectionDAG | Borrow chain |
|---|---:|---:|---:|
| branch on signed `<` | 11–13 | 3–5 | 3 |
| branch on unsigned `<` | 10 | 3–5 | 3 |
| value of signed `<` | 16 | — | 5 |
| value of unsigned `<` | 12 | — | 5 |
| branch on `v < 0` | 11 | 1 | 2 |
| `(v & 0xffffffff) != v` | 16 | 3 | 3 |

The borrow-chain branch counts assume a flag-only `SBC`; add one for
the `MOV`.  Constants need no chain at all: `v < 0` is
`cmp hi,0; blt`, and a right-hand side with a zero low half reduces
to a high-word compare.

Reach (static, 2026-09-13): `libsqlite3` has 470 on variable
operands and about 130 against constants.  That includes two per
`sqlite3BtreeMovetoUnpacked` bisection step (rowid against cell key),
`vdbeRecordCompareInt`, and 42 in `sqlite3VdbeExec`.  The chain saves
about 20 cycles per compare on penumbra1 and 9 on penumbra2_5.  The
kernel's `off_t`, `time_t` and 64-bit counter code has the same shape;
not counted.

SelectionDAG's `IntegerExpandSetCCOperands` (`LegalizeIntegerTypes.cpp`)
uses `SETCCCARRY` where the target provides it — ARM does — and
special-cases constant halves.  GlobalISel has no counterpart, so this
wants a `doc/llvm-*.md` write-up.

Resolved by naming `SBC` with the flag-only bit `CMPC`
(`instruction-set.md`, conformance test `isa/test_cmpc.s`) and
legalizing an s64 relational `G_ICMP` into the target opcode
`G_ICMP_PAIR`: `CMP` on the low words and `CMPC` on the high, read by
a branch, a carry read or a `SELECT_FLAGS_GPR` diamond.  Greater-than
and less-or-equal swap their operands or step a constant (`x > -1` is
`x >= 0`), a constant with a zero low word leaves a compare of the high
words, and equal high words drop the `CMPC`.  A branch on `<` is now 3
instructions, signed or unsigned; the value of `<` 5; `v < 0` and the
truncation check 2.  In the `test-compiler` corpus at `-O2` the
instructions drop from 380,088 to 344,334, most of it in
`UnitTests/Float/classify.c`, a soft-float classifier (208,331 to
174,301); the rest of the corpus drops 1.0%.  The kernel, `libc.so`
and Doom are not remeasured.  Coverage: `icmp-i64.ll`,
`legalize-icmp-s64.mir`, `select-icmp-pair.mir` and the execution test
`penumbra-codegen/icmp64.c`.

Left:
- A 64-bit select, `max` and `min` among them, picks each word in its
  own diamond and repeats the compare: 9 instructions where one
  diamond takes 5.
- An all-ones constant high word is loaded with `LLIS`, since `CMPC`
  has no immediate form.
- s128 compares keep the generic select chain.
- The upstream write-up above.

## Compiler: signed sub-word loads in relational compares miss LDBS/LDHS

Re-measured and re-scoped 2026-06-22. The original framing (sext through
a `G_PHI`, mirroring `penumbra_zextload_promote`) is **superseded**:
upstream's `combines_for_extload` (in the post-legalizer list) now folds
`G_SEXT(G_LOAD)` → `G_SEXTLOAD` for the single-use, through-`G_PHI`
(including divergent-merge), and mixed sext+zext-consumer cases — all
verified emitting `LDBS`/`LDHS` (or `LDB` + a single mask) via `llc`. The
eq/ne char compares that were most of the 2026-06-09 Dhrystone finding
fold to plain `LDB`, so the Dhrystone binary now carries **zero** adjacent
sext chains.

The residual gap is narrower: a **signed relational** compare of a
sub-word load inside a loop. The canonical
`while (*p > 0 && *a == *b)` still emits `LDB; SHL r,24; SAR r,24` for the
`*p > 0` test (the `*a == *b` eq part folds to `LDB`) — the legalizer
widens the signed `G_ICMP` with `G_SEXT` and `combines_for_extload` does
not fold it to `G_SEXTLOAD` in that loop-carried shape, where the
isolated shapes do fold. Why the in-loop relational case bails when the
isolated cases fold is the thing to diagnose before writing any rule.

**Measured opportunity (static, register-checked: a `ldb`/`ldh`
immediately followed by a width-matched `shl;sar` on the same reg, which
`LDBS`/`LDHS` collapses 3→1):** kernel 41 (3 byte + 38 half), libc.so 20
(all byte), `sh` 0, Dhrystone 1. A lower bound (non-adjacent foldable
sites exist), but the immediately-adjacent count is small and ~2
instructions each — **low value**, consistent with the original "low
priority" tag. Not a hot-path lever; pick it up only if the relational
diagnosis turns out cheap, or if a kernel bench surfaces one of these in
a hot loop.

**Re-ranked 2026-09-13: the reach is in sign-bit tests, and it is
hot.**  The count above only matched a load immediately followed by
`shl; sar`.  Testing the sign bit of a byte or halfword (`b & 0x80`,
`(int8_t)b < 0`) emits the widened comparison against a materialized
−1, with other instructions in between:

```
ldb r3,[r1+1]; llis r4,-1; mov r11,r3; shl r11,24; sar r11,24; cmp r11,r4; ble
```

`ldb; test r3,128; bne` (or `ldbs; cmp r3,0; blt`) does the same in
three, often with one fewer callee-saved register.

- **SQLite:** 428 sites in `libsqlite3` (377 byte, 51 halfword) — one
  per varint and record-header byte, in `sqlite3GetVarint*`,
  `btreeParseCellPtr`, `sqlite3VdbeRecordCompare*` and
  `sqlite3VdbeExec`.
- **Doom:** the BSP walk tests `bspnum & NF_SUBSECTOR` on an i16
  child index as `llis; mov; shl 16; sar 16; cmp; bgt` where
  `test r1,#0x8000; beq` does it — once per node.
- **Kernel:** the `sy_narg` compare (`ldh; mov; shl 16; sar 16`) runs
  on every syscall.

A post-legalizer rule rewriting a sign test of `G_SEXT_INREG` into a
test of the sign bit covers the sign-bit cases.  The −1 is a separate
gap, in "compare and branch selection misses immediate and TEST
forms".  A related narrowing: `ldw r; sar r,16` with only the high
half used is `ldhs [+2]` — SelectionDAG narrows the load, GlobalISel
does not; 15 Doom sites, including `R_PointOnSide`.

## Compiler: selectAddSubCarry misses the immediate fold — RESOLVED

`selectAddSubCarry` emitted the register `ADD`/`ADC`/`SUB`/`SBC` for
the i64 carry chain whatever the second operand was.  Every 64-bit
counter bump — `cpu_nsyscall` on every syscall, `cpu_nintr` and the
`evcnt` counters on every interrupt, the TCP/IP statistics — carried
an `lli 1` for the low word and a `mov rz, zero` for the high one:

```
lli r4,1; mov r11,zero; add r4,r2; adc r3,r11
add r2,1; adc r3,zero
```

A small i64 subtraction was worse: `sub_to_add` turns `x - 1000` into
`x + -1000` before narrowing, giving `llis -1000; llis -1; add; adc`.

The constant now goes into the instruction when a form exists that
leaves the same result and flags: `ADDi`/`SUBi` for a uimm16 constant,
the opposite family's immediate for a negated one, `R0` for a zero
carry-in operand, and `R0` under the opposite opcode for an all-ones
one.  Shape coverage: `GlobalISel/select-add-sub-carry.mir`.

Measured statically on the hot-path review corpus (2026-09-13): kernel
−1,302 instructions (`.text` −5,208 B, 37 fewer callee-saved saves),
`libsqlite3` −450, Doom unchanged.  `syscall` −2, `intr_dispatch` −5,
`statclock` −14, `binuptime` −2.  The copied zeros feeding an `ADC`
went from 577 to 7 in the kernel.  Thirteen kernel functions grew, the
largest `ip_output` by 28 of 1,578 instructions, from the allocator
choosing differently once the constants' registers were gone.

## Compiler: redundant extension of comparison results

The branchless `icmp`-to-value path (`selectICmpToValue`) already
produces a clean 0/1 in a GPR, but the extension consumers re-process
it from scratch:

- `zext i1` -> `selectZExt` emits `ANDi #1`, a no-op on an already-0/1
  value.  The inverted predicates (`ult`, `ugt`, `ne`) compound it:
  `emitCarryToValue`'s own `ANDi #1` (masking the SBC's 0/0xFFFFFFFF down
  to 0/1) is then followed by the zext's `ANDi #1`, masking twice.
- `sext i1` -> `selectSExt` emits `SHLi #31` + `SARi #31` to turn 0/1
  into 0/-1, when the flag read could have produced the 0/-1 mask
  directly (a single SBC of zeros after the compare yields -(NOT C);
  pick the polarity to land 0/-1) and skip the two shifts.

The zext half is done: `selectZExt` emits a `COPY` (which the register
coalescer folds away) when the `G_ZEXT` source is a `G_ICMP`, dropping the
redundant mask.  Code in
`llvm/llvm/lib/Target/Penumbra/GISel/PenumbraInstructionSelector.cpp`.

Two pieces remain, both measured low-value 2026-06-06:

- **zext through a `G_PHI`.**  The `COPY` fold only fires when the icmp is
  the *direct* def, so `zext(phi(icmp, icmp))` still double-masks — 12
  cold kernel sites (all `and r,1; and r,1`).  Extending `selectZExt` to
  walk the PHI would have to prove every incoming value is 0/1, which is
  awkward at selection.  The idiomatic fix is instead a post-ISel
  `MachineFunctionPass` collapsing `ANDi x,C1; ANDi x,C2 -> ANDi x,C1&C2`
  (the redundancy only exists after selection, so a generic combiner
  cannot see it).  That is the same vehicle `RISCVOptWInstrs` (redundant
  `sext.w`) and `X86FixupSetCC` (zext-of-setcc) use for
  selection-introduced redundancy — but those target pervasive patterns;
  a pass for 12 cold instructions is not justified.  Reach for it if the
  pattern ever becomes frequent.

- **sext.**  `selectSExt` would have to *consume* the icmp (selection is
  bottom-up, and with no single-instruction negate, converting a 0/1
  afterward costs the same `SHLi;SARi` it would replace) and pick the
  carry polarity per predicate to land 0/-1 — a silent-miscompile
  surface.  Measured reach: 1 site in Dhrystone, 87 cold sites in the
  NetBSD kernel (0.23% of comparisons, ~0.02% of code).  Low leverage for
  the complexity; deferred.

Do **not** try to drive this from `setBooleanContents(ZeroOrOne)`.
Measured 2026-06-06: the redundant masks are emitted at *selection*,
after the combiners run, so boolean content cannot fold them.  Worse,
declaring `ZeroOrOne` flips `LegalizerHelper::lowerThreewayCompare` from
the SELECT-chain expansion to the arithmetic `zext(gt) - zext(lt)` form,
regressing the qsort i32 comparator (`scmp_i32` 10→15, `ucmp_i32` 10→13
instructions) while only shrinking the rarely-used i64 three-way compares
(−4 each); no other CodeGen test moves.  Only revisit `ZeroOrOne`
alongside a custom `G_SCMP`/`G_UCMP` lowering that keeps the i32
SELECT-chain.

## Compiler: flag definitions stay live after C++ selection

An instruction selected through a TableGen pattern gets its unused
`SR` definition marked dead (`implicit-def dead $sr`); one built by
hand in `PenumbraInstructionSelector.cpp` keeps it live
(`implicit-def $sr`).  MachineLICM and MachineSink refuse to move an
instruction whose physical-register definition is live, so every
hand-built ALU operation stays where selection put it even though it
has no side effects.

- **PIC addresses in loops.**  In "PIC global addresses are
  recomputed inside loops", the `PICLLI`/`PICLUI` carriers now leave
  the loop but the `PICADDPC` anchor stays, behind a copy of the
  hoisted offset: 2 instructions per iteration where hoisting the
  anchor too needs none.
- **Divides.**  `DIV`'s live `SR` definition was what kept
  MachineLICM from lifting a guarded divide out of its guard, before
  the divides were marked side-effecting.
- **Multiplies in a select's arm.**  `penumbra_select_of_identity`
  leaves `x * select(c, 1, k)` alone because `MUL` cannot sink into
  its arm; once its flag definition is dead, `G_MUL` with 1 joins the
  rule.

Fix: mark the `SR` definition dead on each hand-built instruction
whose flags nothing reads.  No selector path lets a flag value cross
a block boundary before register allocation — a compare is emitted
beside its branch, a carry fuses only with the instruction just before
its user, and a select diamond emits its own compare beside its
branch — so "not read before the next `SR` definition or the end of
the block" is the whole test.

## Compiler: large frame offsets write the flags unchecked — RESOLVED

`PenumbraRegisterInfo::eliminateFrameIndex` handles an offset beyond
the 16-bit field by building `LLI; LUI; ADD` into a scavenged
register in front of the load or store.  The `ADD` writes the flags,
and nothing checked that they were dead there: a spill reload with a
large offset between a compare and its branch, or inside a
64-bit `ADD`/`ADC` carry chain, would have corrupted the flags.  It
needs a frame over 32 KB, and no failure was known.  User code cannot
save the flags around the `ADD` (`RDSPR` is privileged and `WRSPR SR`
is reserved).

Resolved by forming the address ahead of the instruction that starts
the live flag value where the flags are live at the access
(`frame-index-flags.mir`).  Selection keeps every flag value inside
one block and the frame base register is not written while flags are
live, so the two cases that would defeat that — flags live into the
block, or the base written in between — end compilation with a fatal
error rather than miscompile (`frame-index-flags-error.mir`).  No site
in the kernel, libc, Doom or the `test-compiler` corpus had the flags
live, and the corpus compiles to identical code.

The 43 large-frame sites in `libc.so` are all callee-saved saves and
restores, in three functions with big local buffers (`__put_page`,
`cdbw_output`, `allocaddrinfo`): the save slots sit at the far end of
a frame over 32 KB, so each save and restore costs `LLI; LUI; ADD`.
Placing them next to SP would remove that; three functions do not make
it worth doing yet.

## Compiler: countdown loop exits pay for an overflow flag — RESOLVED

`do { ... } while (count--)` loops — all eight Doom column and span
drawers — ended in eight instructions where two do:

```
lli r6,1; add r1,r6; mov r6,lr; adc r6,zero; lli r7,1; xor r6,r7; and r6,1; bne loop
add r1,1; bne loop
```

`lr` held a zero for the whole loop, and `r7` was an extra
callee-saved register spent on the sequence.

LSR turns the down-counter into an up-counter whose exit test is
`iv + 1 == 0`, and CodeGenPrepare rewrote that test as
`uadd.with.overflow`.  Selection has no branch on an overflow op's
carry, so it materialized the carry, inverted it through the
branch-inversion `G_XOR`, masked it and tested it.

`PenumbraISelLowering` now refuses the rewrite in
`shouldFormOverflowOp` for register-width types.  The compare selects
next to its branch, and compare elimination reuses the add's flags.
Wider adds keep the overflow form: it narrows onto the `ADD`/`ADC`
chain, whose last carry is the wrap, while the compare would expand
as in "64-bit relational compares expand to select diamonds".
Refusing both widths cost `bintime_addx` — inlined into `binuptime`,
`nanouptime` and `microuptime` on every `clock_gettime` — 7 to 10
instructions and extra branches.  That compare is now `CMP` + `CMPC`
and a branch, so refusing the wider form is worth measuring again.  Branching on the carry
from inside the selector was rejected: the instructions between the
add and the branch are still generic there, so whether SR survives
would have to be guessed from how they will select.  Selection
places flag producers beside their consumers; flag reuse belongs to
the peephole pass, which sees each instruction's real SR defs.
Shape coverage: `wrap-check-branch.ll`.

`R_DrawColumn`'s loop is 12 instructions per pixel instead of 18, and
the function saves 3 registers instead of 4.  Together with the
inverted-boolean fix in "compare and branch selection misses
immediate and TEST forms", measured statically on the hot-path review
corpus (2026-09-13):

| | `.text` | callee-saved saves | carry reads (`adc x, zero`) |
|---|---:|---:|---:|
| Doom | −844 B | 4,664 → 4,656 | 145 → 134 |
| kernel `GENERIC.DEBUG` | −10,144 B | 35,947 → 35,937 | 1,199 → 1,146 |
| SQLite (PIC) | −1,544 B | 14,077 → 14,075 | 806 → 794 |

A general wrap check, `a + b < a`, selects as `add; cmp sum, a; bcc`,
and compare elimination then drops the compare: the sum is below an
addend exactly when the add wrapped, so the add's C is the compare's
inverted, and readers of C alone swap `BCC`/`BCS`.  On the corpus it
fires only in the kernel, in 4 functions.  Shape coverage:
`compare-elim.ll`.

Open: a wrap check used as a value still selects as
`cmp; mov; sbc zero; and 1` after the add, where `mov; adc zero` does.
One site in Doom.

## Compiler: a conditional add or subtract becomes a select of zero — RESOLVED

`if (c) x -= k` reaches the backend as `x - select(c, 0, k)`.  The
middle end folds the branch into a select, which pays off on a
target with a conditional move.  Penumbra has none, so the select is
lowered back into a branch diamond while the subtract stays
unconditional: the common path now runs a `MOV` of zero and a `SUB`
of it, neither of which the original branch needed.

```c
unsigned w(unsigned pos, unsigned inc, unsigned lim, unsigned k) {
    pos += inc;
    if (pos >= lim)
        pos -= k;
    return pos;
}
```

```llvm
%7 = select i1 %6, i32 0, i32 %3
%8 = sub i32 %5, %7
```

becomes `mov r2, zero; cmp r1, r3; bcc .L; mov r2, r4; .L: sub r1,
r2`, where the source asked for `cmp r1, r3; bcc .L; sub r1, r4; .L:`.
In the audio mixer it is 2 of the 19 instructions per voice-sample:
the loop wrap of every voice.

Resolved by `penumbra_select_of_identity` in the pre-legalizer
combiner, the GlobalISel counterpart of SelectionDAG's
`foldSelectWithIdentityConstant`: `x op select(c, id, k)` →
`select(c, x, x op k)` for `ADD`, `SUB`, `OR`, `XOR`, the shifts and a
pointer offset with 0 and `AND` with -1, the select on either side of
a commutative operation.  Machine sinking then moves `x op k` into its
arm.  The mixer's loop wrap is `cmp; bcc; sub` with the subtract on
the wrap path only, 17 instructions per voice-sample where it was 19
(`test/CodeGen/Penumbra/select-of-identity.ll`).

It runs before legalization on purpose.  Placed after it, it also
reshaped the selects the legalizer expands a 64-bit shift into, and
`fshl_i64` grew by two callee-saved spills.  `MUL` with 1 is left out:
its selection keeps its flag definition live, so the multiply cannot
sink into its arm and the rewrite only adds copies around it (see
"flag definitions stay live after C++ selection").

The compare-materialisation entries,
"signed comparisons have no branchless materialisation" and "64-bit
relational compares expand to select diamonds", are the same missing
conditional move seen from the other side.

## Compiler: InstCombine's shift-and-mask form costs a constant

InstCombine canonicalizes `(x >> C1) << C2` with `C1 > C2` into one
shift and a mask, `(x >> (C1 - C2)) & -(1 << C2)`.  On x86 and AArch64
the mask is a single instruction.  Penumbra's `ANDi` takes a
zero-extended 16-bit immediate, so a mask of high ones becomes
`llis t, -(1 << C2); and t, x`: two instructions and a scratch
register where a second shift is one.

Found in the fractal demos (2026-09-13).  `qmul(zr, zi) * 2` —
`trunc((a * b) >> 28) << 1` — came out as `lshr i64 %p, 27`, `trunc`,
`and i32 %t, -2`: five instructions per iteration after i64 narrowing,
where `shr lo, 28; shl hi, 4; or; add x, x` is four, in a loop with no
free register for the constant.  The demos now write 2ab as one shift
(`qmul2`) and no longer show it.

Fix: a Penumbra combine rewriting `G_AND (G_LSHR y, c), -(1 << k)`,
also through a `G_TRUNC`, into `G_SHL (G_LSHR y, c + k), k` when the
mask does not fit the immediate.

The mask on its own is far more common and a different trade.  A
static scan (2026-09-13) finds `llis -2^k; and` 325 times in the kernel,
128 in `libsqlite3` and 11 in Doom, almost none right after a shift —
round-down alignment masks.  `shr x, k; shl x, k` matches their
instruction count and frees the scratch register, but overwrites `x`,
so it only wins where `x` is dead afterwards.

## Compiler: Ch()-style bit-select not folded to the masked-merge form

In compiled `SHA256_Transform`, `Ch(e,f,g) = (e&f) ^ (~e&g)` emits the
naive `not`+2×`and`+`or` sequence (5 ops plus copies) while MD5's
equivalent `F(b,c,d)` correctly gets the masked-merge form
`d^(b&(c^d))` (3 ops plus one copy).  Both should canonicalize to the
xor form (InstCombine's bit-select fold); find where the SHA shape
escapes it — likely the `^` vs `|` variant or a disjoint-or rewrite
upstream of the fold.  Worth ~4 instructions × 64 rounds per block.

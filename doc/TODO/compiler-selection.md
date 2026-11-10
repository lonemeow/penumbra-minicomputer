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

## Compiler: what `G_ICMP_PAIR` left of the 64-bit compares

s64 relational compares select as `CMP` + `CMPC` since
`8c53d2bb4b84`.  Left over from that work:

- A 64-bit select, `max` and `min` among them, picks each word in its
  own diamond and repeats the compare: 9 instructions where one
  diamond takes 5.
- An all-ones constant high word is loaded with `LLIS`, since `CMPC`
  has no immediate form.
- s128 compares keep the generic select chain.
- GlobalISel has no counterpart to SelectionDAG's `SETCCCARRY`
  (`IntegerExpandSetCCOperands` in `LegalizeIntegerTypes.cpp`), which
  is why the target needs its own opcode; that wants a `doc/llvm/`
  write-up for upstream.

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

## Compiler: a wrap check used as a value pays for its materialisation

Register-width wrap checks stay compares (`9a9bcc05c70a`) and a branch
on one reuses the add's carry (`ca1d5c8e4d27`).  Left over:

- A wrap check used as a value still selects as
  `cmp; mov; sbc zero; and 1` after the add, where `mov; adc zero`
  does.  One site in Doom.
- Wider adds keep CodeGenPrepare's overflow form, because the
  compare it replaces used to expand into select diamonds.  That
  compare is now `CMP` + `CMPC` and a branch, so refusing the wider
  form too is worth measuring again: `bintime_addx`, inlined into
  `binuptime`, `nanouptime` and `microuptime` on every
  `clock_gettime`, is the site that decided it.

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

## Compiler: a mask of more than 16 low ones is built where two shifts do

`ANDi` takes a zero-extended 16-bit immediate, so keeping a value's
low n bits for n from 17 to 31 builds the mask first: `x & 0xFFFFFF`
becomes `lli t, 65535; lui t, 255; and x, t`, three instructions and a
scratch register.  `shl x, 32 - n; shr x, 32 - n` is two and needs no
register.  Every one of those instructions takes a cycle on gen2.5
and gen3, and the
shift pair is the shape gen4's extension fusion takes as one op (the
ISS pairing model's `fuse:ext`).

A static scan (2026-11-09) finds `lli t, 65535; lui t, 2^k - 1` with
the `AND` directly behind 59 times in the `GENERIC.DEBUG` kernel, 82
in `libc`, 192 in `libsqlite3` and twice in Doom.  The masks with any
use number 263, 270, 265 and 37; most are `0x7FFFFFFF` and
`0x3FFFFFFF`.

Where `x` stays live, the `AND` can write `t` and keep it, and the
shift pair needs a `MOV` ahead of it: three instructions either way,
the scratch register still freed.  Two-shift masks of high ones are
the same trade with a one-instruction mask (this file, "InstCombine's
shift-and-mask form costs a constant").

Open:

- A loop-invariant mask that MachineLICM hoists leaves one `AND` an
  iteration against the shift pair's two.  Selection runs before
  MachineLICM, so either the rewrite comes after it and takes only the
  masks left in place, or it leaves loop bodies alone.
- The static counts say nothing of how often the sites run.

## Compiler: a mask is kept where the shift behind it discards its bits

`(signed char)x` of a `short` argument comes out as `and r1, 65535;
shl r1, 24; sar r1, 24`.  The `AND` clears only bits the `SHL` shifts
out, so it does nothing.

InstCombine writes the narrowing as `zext i16 %x to i32` ahead of `shl
24; ashr exact 24`: IR has no extension that leaves the upper bits
open.  The legalizer makes the `G_ZEXT` a mask.  An `unsigned short`
argument loses it, since its `G_ASSERT_ZEXT` makes the mask known
redundant.  A `short` arrives under `G_ASSERT_SEXT`, and only the bits
the `G_SHL` demands show the mask is dead, the work SelectionDAG's
`SimplifyDemandedBits` does; the GISel combiners keep it.

A static scan (2026-11-09) finds an `AND` of low ones directly ahead
of a `SHL` of the same register that shifts every cleared bit out 17
times in the `GENERIC.DEBUG` kernel, 32 in `libc`, 32 in `libsqlite3`
and twice in Doom.

Open:

- Where the fix belongs: a combine dropping `G_AND x, 2^n - 1` under a
  `G_SHL` by at least `32 - n`, or a demanded-bits combine that also
  covers the other users that ignore high bits.  Not investigated.
- How often the sites run.

## Compiler: Ch()-style bit-select not folded to the masked-merge form

In compiled `SHA256_Transform`, `Ch(e,f,g) = (e&f) ^ (~e&g)` emits the
naive `not`+2×`and`+`or` sequence (5 ops plus copies) while MD5's
equivalent `F(b,c,d)` correctly gets the masked-merge form
`d^(b&(c^d))` (3 ops plus one copy).  Both should canonicalize to the
xor form (InstCombine's bit-select fold); find where the SHA shape
escapes it — likely the `^` vs `|` variant or a disjoint-or rewrite
upstream of the fold.  Worth ~4 instructions × 64 rounds per block.

## Compiler: selects that share a condition are expanded one by one

Each `G_SELECT` becomes its own pseudo, and
`EmitInstrWithCustomInserter` expands each pseudo into its own
diamond: its own compare, branch and join.  Two selects on one
condition therefore compare twice and branch twice, and whatever the
compare reads has to be live, or reloaded, at both.

The middle end produces such pairs from an `if`/`else` that assigns
two variables.  CoreMark's `matrix_sum` is one:

```c
if (tmp > clipval) { ret += 10; tmp = 0; }
else               { ret += (cur > prev) ? 1 : 0; }
```

reaches the backend as `select %c, 0, %tmp` and `select %c, 10, %flag`,
and comes out as

```
ldw  r5, [sp + 32]      ; clipval, spilled
cmp  r6, r5
mov  r7, zero
bgt  1f
mov  r7, r6
1: lli  r5, 10
ldw  r8, [sp + 32]      ; clipval again
cmp  r6, r8
bgt  2f
mov  r5, r3
2:
```

with a third diamond before it building the 0/1 of `cur > prev`.
The three leave no register for `clipval`, which is why it is
reloaded.  The loop is 21.7 instructions an element where the branch
as written is about 11, 3.5 % of CoreMark's instructions
([workloads.md](workloads.md), "hot-loop code review — CoreMark").

Open:

- Expand adjacent select pseudos with the same compare operands and
  condition as one diamond with a PHI each, as RISC-V's
  `emitSelectPseudo` does.
- Whether the middle end should form these selects at all for a
  target with no conditional move: that is the cost question in
  [compiler-pipeline.md](compiler-pipeline.md), "no branch-cost
  model".  Merging helps either way, since selects also arrive from
  source.

## Compiler: a sub-word mask widened by sign extension misses the immediate

An `and` on a narrow type keeps its constant in that type until the
legalizer widens it, and widening sign-extends the constant.  A mask
with the type's top bit set then has ones in all its upper bits, does
not fit `ANDi`'s zero-extended 16 bits, and is loaded into a register:
`and i8 %c, -33` becomes `llis r5, -33` and `and r3, r5`.

Where the other operand's upper bits are known zero, as they are
after `LDB`, the zero-extended mask is equivalent and fits: `and r3,
223`.

CoreMark's `core_state_transition` tests `c == 'e' || c == 'E'` this
way.  The constant is loaded once a call, outside the loop, but it
holds a callee-saved register in a function that has none to spare,
so the cost is a save and a restore a call: 0.5 % of CoreMark's
instructions, and a store the core waits on
([workloads.md](workloads.md), "hot-loop code review — CoreMark").

Open:

- Shrink the constant to the bits the other operand can have set,
  when that makes it fit the immediate: SelectionDAG's
  `targetShrinkDemandedConstant` is the same idea.
- Count the sites in the kernel and `libc`, as the mask entry "InstCombine's
  shift-and-mask form costs a constant" did for its form.

## Ruled out

- **Callee-saved slots next to SP in frames over 32 KiB.**  The save
  slots sit at the far end of the frame, so each save and restore in
  such a frame costs `LLI; LUI; ADD`.  In `libc.so` that is 43 sites
  in three functions with big local buffers (`__put_page`,
  `cdbw_output`, `allocaddrinfo`) — not worth a frame-layout change.

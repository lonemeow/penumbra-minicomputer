# Userland and libc

## Userland: ssh dies with SIGBUS — RESOLVED (lld PLT reach)

Root-caused from the first core dump the port ever wrote: PC held
instruction-encoding garbage loaded through a libcrypto PLT stub.
The stub's PC-plus-two-16-bit-immediates addressing reached at most
~98 KB and lld's split helper silently truncated beyond that;
libcrypto's .plt-to-.got.plt span is ~232 KB, so its every stub
loaded neighboring PLT code bytes as a function pointer.  Fixed in
lld (full 32-bit LLI/LUI displacement; regression
`lld/test/ELF/penumbra-plt-far.s`).  A linker change is invisible to
nbmake's dependency tracking: the fix reaches installed binaries
only through a clean userland rebuild, and any library that large
built before the fix is suspect until relinked.

## Libc: string-routine optimization — strlen word-at-a-time, strcmp/strcpy unrolled (DONE)

The shared string routines in
`common/lib/libc/arch/penumbra/string/{strcpy,strcmp,strlen,memcmp}.S`
are byte-at-a-time.  On Dhrystone they are not a minor cost — they are
most of the program.

Wiring the bare-metal benchmarks to these routines (replacing hand-rolled
byte-loop `string.c` helpers) moved gen2 HW Dhrystone 5.03 → 6.32 DMIPS,
entirely from fewer retired instructions — CPI was flat (3.03 → 3.06).
That win was the word-at-a-time `memcpy` alone: Dhrystone copies one
~48-byte `Rec_Type` per iteration as a `bl memcpy`, and byte → word
fill cut it ~288 → ~93 insns.  The L1-D store delta confirms it
(0.75 × 48 × iters of stores removed).  But the *remaining* per-iteration
count stayed high, which prompted a full attribution.

Instruction-count profile — deterministic, ISS-traced over 100
steady-state iterations (architectural counts, not timing): **727
insns/iter.**  Method is reusable and noted at the end.

| function                          | insns/iter | share |
|-----------------------------------|-----------:|------:|
| strcpy (byte loop, 6 insns/byte)  |      189.0 | 26.0% |
| strcmp (byte loop, 9 insns/byte)  |      178.0 | 24.5% |
| dhrystone_main (incl. inlined Proc_1–5) | 138.0 | 19.0% |
| memcpy (word loop, *called* from Proc_1/3) | 100.0 | 13.8% |
| Proc_8 (2-D array index)          |       62.0 |  8.5% |
| Proc_6                            |       22.0 |  3.0% |
| Func_2 / Proc_7 / Func_1          |       38.0 |  5.2% |

The three mem/string library routines are **64%** of every iteration;
`strcpy`+`strcmp` alone are **51%**.

The dynamic opcode mix shows the shape: `add` 157/iter (22%, mostly
pointer bumps — Penumbra has no post-increment addressing, so every
copy/compare step needs an explicit `add`); `ldb`+`stb` 109 (15%, byte
traffic); compare+branch (`test`/`cmp`/`bne`/`beq`/`b`) ~200 (28%, the
byte-loop control).  The genuinely ISA-structural tax these loops get
blamed on — 2-operand `mov` (49/iter) plus address materialization
`lli`+`lui` (40/iter, no gp-relative addressing) — is only ~12%, not the
main cost.

Reference point: a typical RV32 Dhrystone runs ~331 insns/iter, but it
links a libc whose strcpy/strcmp/memcpy are word-at-a-time and inlines
the small constant-size memcpy.  Most of the 2.2× gap is library
algorithm, wearing the costume of an ISA gap.

Outcome (HW-validated):

Word-at-a-time was tried for all three routines, then kept only for
`strlen`.  For `strcmp`/`strcpy` it *regressed* Dhrystone (750 vs 736
insns/iter): Penumbra has no unaligned access, so the word path needs
both operands co-aligned, and Dhrystone's are not — `strcmp(Str_1_Loc,
Str_2_Loc)` compares two adjacent `char[31]` stack arrays the compiler
packs at byte alignment, so the word path never triggers and the
routine just paid its setup (align prologue + two `LLI`+`LUI` constants
+ callee-saved spills) for a byte fallback.

Cross-platform practice settles it: the strict-alignment arches (mips,
m68k, sparc32) keep `strcmp`/`strcpy` as byte loops; only x86 word-
optimizes them, and only because cheap unaligned access lets it align
just one side.  Penumbra is in the strict-alignment camp.  `strlen`,
with one pointer and always-reachable alignment, is word-at-a-time on
nearly every optimized arch.

Resolution:
- `strlen` — word-at-a-time, frame deferred past the align prologue so
  a head-NUL returns without spilling.  Zero-byte lane located by
  shifting the detector mask's 0x80 to the sign bit (Penumbra has no
  CLZ; cf. sparc64's byte-wise mask scan).
- `strcmp`/`strcpy` — byte loops, unrolled 4× with offset addressing
  (m68k precedent), redundant trailing branch removed.

Real-HW results (gen2 FPGA):
- Dhrystone: 6.32 → 7.11 DMIPS (+12.5%), −242 cycles/iter, from the
  `strcmp`/`strcpy` unroll.
- `strlen` length sweep (pbench): +11% on len-8 (setup cost), but −30%
  at 64 B and −41% at 4 KiB.  Crossover ~len 12–16.

The instruction saving (−94/iter) and cycle saving (−242/iter) are
consistent across ISS, RTL sim, and HW; only the baseline differs.
Caution learned the hard way: the prebuilt RTL sim was stale (an older,
slower gen2 whose retire counter over-counts flushed slots, reading
~4× the instructions), and under-reported the win as −2.4% where HW
showed −10.7%.  Treat a sim as ground truth only if it is current; the
FPGA always is.

These optimizations *appreciate* on gen2.5.  The cycle saving is a
roughly fixed amount of removed work; gen2.5's lower flush/hazard
stalls shrink the baseline it is measured against, so the same saving
is a larger fraction.  `strlen`'s word loop is itself hazard-bound
today — the `haszero` chain (`sub`→`and`→`and`) is a dependent ALU
sequence an in-order scalar core cannot overlap, so it runs ~1.7×, not
the 4× the stride implies; that gap closes as gen2.5 hides hazards.

Correctness for all three is covered by a bare-metal test
(`benchmark/strtest`) over every start alignment × length.

Still open: inline constant-size `memcpy` in the backend.  Dhrystone's
48-byte `Rec_Type` copy lowers to `bl memcpy` (~100 insns/iter incl.
call + prologue); inlining ~12 word load/stores would drop it to ~25.
Complements the word-fast routine, which still serves the variable-size
case.

Profiling method (reusable): trace a short run with `+trace`, find a
loop-carried PC in the measurement function (exactly one hit per
iteration), then bucket trace PCs by symbol address-range between two
mid-run occurrences of that marker.  This isolates steady-state from
one-time setup/reporting and yields exact per-function and per-opcode
instruction attribution.

Related: "Libc: memcpy misses same-offset misaligned shortcut" and
"Libc: cache-line-align the memcpy/memset hot loops" tune the same
routine directory along the variable-size and code-placement axes.

## Libc: memcpy misses same-offset misaligned shortcut

NetBSD's libc memcpy on Penumbra takes the byte-fallback path for
*all* misalignment configurations, even the cases where a word-at-
a-time body would still work.  Measured via `pbench libc memcpy_align`
at n=256 (commit 3decf13fa4b0):

```
  aligned (size=256, main sweep):    ~83 us
  src=1, dst=0:                     ~287 us
  src=0, dst=1:                     ~287 us
  src=1, dst=1 (same offset):       ~290 us  ← could be word body
  src=1, dst=3 (different offsets): ~290 us  ← byte fallback only
```

When src and dst are misaligned by the same offset, after a 3-byte
head prologue both pointers reach word boundaries simultaneously
and the middle can run word-at-a-time.  Current libc misses this
case and lumps it with the genuinely-impossible different-offset
case.

Implementation lives in
`common/lib/libc/arch/penumbra/string/memcpy.S`.  An offset-
comparison check at the top would let the same-offset case fall
into the aligned word loop.

Expected win: ~3.4× on same-offset misaligned calls (~290 us → ~85 us
matching aligned).  Different-offset path is unchanged.  Affects
roughly half of all "misaligned" memcpy calls in practice (anything
where src and dst come from the same allocation pool or share a
base alignment).

## Libc: cache-line-align the memcpy/memset hot loops

The 2026-06-13 pbench snapshot showed the small-size `memcpy` rows
(size=1/16/64, and `memcpy_align` n=7/31) regressing while small-size
`memset` *improved* — across a pure compiler rebuild that touched no
libc source.  Diagnosis: the routines are hand-written assembly
(`common/lib/libc/arch/penumbra/string/{memcpy,memset}.S`), so their
instruction bytes are fixed; only their *placement* moved.  The L1
I-cache is 1 KiB **direct-mapped** with 16-byte (4-word) lines, and
neither `.S` carried any `.p2align` — so a rebuild relocates the fixed
routine bytes and the hot loops land at new, arbitrary offsets relative
to the line grid (and to whatever caller they share a set with).  The
memset-up / memcpy-down anti-correlation is the layout-roulette
signature.

Cross-arch check: penumbra was the *only* port declaring these routines
with bare `.globl`/`.type`, bypassing its own `ENTRY()` macro.  Peers get
at least the macro's entry alignment, and the hand-tuned ports (sparc64,
aarch64, sh3) explicitly `.p2align` their inner loops — sparc64 literally
comments `.align 32  ! ICache align.`.

Fixed in commit f0a9e9e0ec9a: `.p2align 4` on the function entries and
the hot loops — memcpy `.Lword` + `.Lbyte` (the byte loop is the whole
copy when operands are misaligned, so it is hot), memset `.Lword` only
(its `.Lhead`/`.Ltail_loop` run ≤3 times).  The loops now land on
16-byte boundaries at every build, so hot-loop placement is
deterministic and the small-size rows stop swinging on unrelated
code-size changes — consistency was the goal, restoring the benchmark's
power to detect real libc regressions.

HW result (min trial, vs the regressed 06-13 baseline): the fix recovers
the iteration-scaling part of the regression, and recovers *more* the
more the loop iterates — memcpy size=1 8.19→7.50 us (~23%), size=16
12.14→10.52 (~43%), memcpy_align n=7 11.90→10.51 (~38%), n=31
17.75→16.40 (~46%), n=127 41.25→39.95 (~full).  memset unchanged.  This
is the predicted split: `.p2align` removes the per-iteration line
straddle (so the win grows with trip count); the residual small-size
floor is the per-call **cross-routine set conflict** (routine vs. caller
congruent mod 1 KiB in the direct-mapped index), which `.p2align` pins
only the low 4 bits and so cannot touch — only associativity can (the
4-way L2, the gen2 cache direction).

Follow-up — the *compiler* has the same gap for compiler-generated code;
see "Compiler: align functions and hot loops in codegen" in compiler-pipeline.md.

## NetBSD/OpenSSL: hash message ingestion byte-gathers even when aligned

Word loads must be 4-aligned (`VEC_ALIGN`), so the portable
byte-assembly in OpenSSL's `HOST_c2l` (MD5: 4×`ldb`+3×`shl`+3×`or` =
10 instructions per word, 160 per block ≈ 14% of the block cost) and
in libc sha2's `be32dec` path is semantically forced for arbitrary
`unsigned char *` input — the compiler cannot legally merge it.  A
source-level aligned fast path (word loads when `(p & 3) == 0`, which
is the overwhelmingly common case for hash input buffers) is the same
dispatch a hand-written implementation would use.  Decide per-library
whether the reachover/MD configuration offers a clean hook before
touching dist code.

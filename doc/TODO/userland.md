# Userland and libc

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

## Ruled out

- **Word-at-a-time `strcmp` and `strcpy`.**  Tried and measured
  slower on Dhrystone (750 vs 736 instructions per iteration): with no
  unaligned access the word path needs both operands co-aligned,
  which the common case is not.  They stay unrolled byte loops, like
  the other strict-alignment ports; the reasoning is in the headers of
  `common/lib/libc/arch/penumbra/string/{strcmp,strcpy}.S`.

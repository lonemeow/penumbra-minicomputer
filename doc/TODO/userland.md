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

## Libc: string routines are byte loops sized for gen2.5

`strcmp` and `strcpy`, and `memcpy` when its operands are misaligned,
run unrolled byte loops
(`common/lib/libc/arch/penumbra/string/`). That shape was chosen on
gen2.5: more heavily unrolled variants, and `str*` routines unrolled
into word loads, lost overall there, because its 4 KiB L1s and
expensive misses charge more for the extra code than the fewer
instructions save. gen3 and gen4 weigh the two the other way.

- **gen3** has 8 KiB L1s in front of the same 64 KiB L2 on the ULX3S
  (256 KiB on the Nexys Video), with cheaper L1 misses, so a longer
  routine takes half the share of the L1-I and costs less per miss.
- **gen4** issues every load, store and branch in its Y slot, one
  access and one branch a pair
  ([gen4 issue pairing](../internals/penumbra4/issue-pairing.md#design)).
  The byte loops are nearly all Y work: X takes only their pointer
  adds. A word loop moves the work into ALU ops, which X pairs for
  free: the zero-byte test `(w − 0x01010101) & ~w & 0x80808080`, and a
  shift-merge (`shr`, `shl`, `or`) when the operands' alignments
  differ.

Hand counts from the pairing model's rules, for long strings in the
steady state, without setup or load-use waits:

| Routine | gen3: instructions a byte | gen4: issue cycles a byte |
|---|---|---|
| `strcmp`, byte loop | 6.5 | 4.0 |
| `strcmp`, words, co-aligned | ~2.8 | ~1.0 |
| `strcmp`, words, shift-merge | ~4.9 | ~1.6 |
| `strcpy`, byte loop | 4.75 | 3.25 |
| `strcpy`, words, co-aligned | ~2.5 | ~0.9 |
| `strcpy`, words, shift-merge | ~3.75 | ~1.3 |

On gen3 the shift-merge path saves 20–25 % a byte, which setup and
footprint can take back on short strings; on gen4 it saves about
60 %. On Dhrystone (2026-11-11 build) `strcpy`, `strcmp` and `memcpy`
are 58 % of the timed loop's 599 instructions per iteration.

One libc serves every generation, and gen3 is the floor it protects.

Open:

- Prototype word `strcmp` and `strcpy` with a co-aligned path and a
  shift-merge path, and `memcpy`'s misaligned path the same way, and
  check them with `make test-libc` at every alignment.
- Measure Dhrystone and a sweep of lengths on `+pairmodel` for gen4
  and `+loadusemodel` for gen3, then on gen3's hardware.
- Measure what real workloads pass: the length of a call per routine
  from `+pcprof` (loop-body counts over entry counts) on `pbench` and a
  shell loop, and the routines' I-cache misses from `+cachemodel` at
  gen3's geometry. A cold call pays for every line it touches, and the
  word paths touch more.
- Software-pipeline the word loops, so the zero-byte test does not wait
  on its load, as `strcpy`'s byte loop does.
- If the generations need different routines, the selection belongs
  with "Compiler: per-generation tuning — models and their consumers"
  in compiler-pipeline.md.

## Libpthread: an uncontended mutex is mostly calls

Linking libpthread replaces libc's stub mutexes before `main()`, so a
threaded process runs `pthread_mutex_lock` and `pthread_mutex_unlock`
for every lock it takes, libc's own (stdio, malloc) among them.
`pbench-pthread pthread mutex` times one uncontended pair: on the
ULX3S gen3 at 37.5 MHz (2026-11-22), 5.97 µs, 228 cycles and 137
instructions a pair, at CPI 1.67 with 22.8 % of cycles in flush
stalls.

Counted from the binaries, the path the dynamically linked benchmark
runs is 134 instructions and 23 taken control transfers:

| Part | Instructions |
|---|---|
| The benchmark's loop and its two PLT stubs | 17 |
| `pthread_mutex_lock` and `pthread_mutex_unlock` | 77 |
| libpthread's PLT stubs, five calls into libc | 25 |
| `atomic_cas_ptr`, twice: the RAS sequence | 12 |
| `membar_exit` once and `membar_enter` twice | 3 |

The work is two 6-instruction compare-and-swaps, the magic check, the
owner read from TLS and the waiter test; most of the rest is getting
to it.

- **The barriers do nothing and cost 21 instructions and 9 of the
  transfers.** libc builds every `membar_*` as one `jmp lr`
  (`membar_ops_nop.c`), and libpthread calls three a pair, each
  through a PLT stub. Declaring `PTHREAD__ATOMIC_IS_MEMBAR` in
  `lib/libpthread/arch/penumbra/pthread_md.h` drops the calls, and
  follows from what libc's empty barriers follow from: the
  architecture needs no barriers
  ([Memory Ordering](../system/architecture.md#memory-ordering)), so
  every atomic operation already orders as one would. sh3 and m68k declare it too, as ports that
  will not go SMP.
- **Each compare-and-swap reaches libc through the PLT**: 5
  instructions and an indirect jump each, 10 a pair. A copy of the
  sequence in libpthread, called directly, skips the stub, but it is
  a second `rasctl(2)` region, and `userret()` walks the region list
  on every return to user mode from an interrupt or exception
  (`sys/arch/penumbra/include/userret.h`).

Open:

- Run `pbench-pthread-static` on the board beside `pbench-pthread`.
  The static link calls the same routines with no PLT stubs, which
  prices the stubs in cycles rather than instructions. The
  benchmark's `all` target builds it; `overlay` installs only the
  dynamic binaries.
- Declare `PTHREAD__ATOMIC_IS_MEMBAR`, and time the pair before and
  after.
- Weigh a libpthread-local compare-and-swap against what a second RAS
  region adds to every interrupt return.

## Ruled out

- **Word-at-a-time `strcmp` and `strcpy`.**  Tried and measured
  slower on Dhrystone (750 vs 736 instructions per iteration): with no
  unaligned access the word path needs both operands co-aligned,
  which the common case is not.  They stay unrolled byte loops, like
  the other strict-alignment ports; the reasoning is in the headers of
  `common/lib/libc/arch/penumbra/string/{strcmp,strcpy}.S`.  This
  weighed a co-aligned-only word path by path length; a shift-merge
  word path weighed on gen3's and gen4's costs is open under "Libc:
  string routines are byte loops sized for gen2.5".

# Compiler

The LLVM backend work is split by area:

- [compiler-selection.md](compiler-selection.md) — compares, flags,
  booleans, extensions
- [compiler-addressing.md](compiler-addressing.md) — global
  addresses, PIC, constants, rematerialization
- [compiler-arith.md](compiler-arith.md) — multiply, divide, strength
  reduction
- [compiler-pipeline.md](compiler-pipeline.md) — passes, frame and
  call lowering, legalization, per-generation tuning

Workload-driven code reviews that span compiler and source changes
live in [workloads.md](workloads.md).

The ranking below orders the open compiler levers.

## Compiler: where the remaining codegen wins are — ranked 2026-09-21

A static scan of built code sized the open codegen items against each
other, so picking among them need not start with a new measurement.

Corpora, all built by the compiler at `98f1233ac22f`: the
`GENERIC.DEBUG` kernel (697,401 instructions), `libc.so` (404,735,
PIC), the full Doom binary (103,302) and the `test-compiler` corpus at
`-O2` (380,088; gcc torture tests, heavy in 64-bit arithmetic).  The
changes since the first ranking took the kernel from 712,161
instructions (−2.1%) and `libc.so` from 409,542 (−1.2%).  Each item is
a code shape counted per site, within a block, in `llvm-objdump -d`
output.  The counts are static, not cycles, and the debug kernel
inflates its diagnostic paths.  There is no column for sites in loops:
in placed code a backward branch is not a reliable loop marker, and
the first ranking's "in loops" figures and larger copy counts came from
a scan that misread branch targets written in upper-case hex.

| Item | Shape counted | Kernel | libc | Doom | Tests |
|---|---|---:|---:|---:|---:|
| "InstCombine's shift-and-mask form costs a constant" | `llis t, -2^k; and y, t` | 413 | 300 | 12 | 234 |
| "inline small constant-size memcpy/memset/memmove" | `lli r3, N` before a call to one, N ≤ 32 (≤ 64) | 510 (654) | — | — | — |
| "pointer-minus-constant and integer MMIO addresses miss their folds": MMIO | `add x, imm; ld/st [x + 0]`, `x` dead | 63 | 2 | 1 | 0 |
| same entry: `p - K` | `llis t, -K; add y, t`, `t` dead | 47 | 63 | 4 | 0 |
| same entry: run-time indexed local | `mov x, sp; [add x, imm;] add x, y; ld/st [x + 0]` | 35 | 32 | 9 | 16 |
| "hot-loop code review — software audio mixing": zero-low-half carry | `add/sub r, 0` feeding `adc`/`sbc` | 2 | 13 | 0 | 1,489 |

The memory-call row counts the kernel only: libc and Doom call through
the PLT, which the scan does not resolve.

Each row saves about one instruction per site, except an inlined
memory call, which trades the call and the callee's loop for a few
loads and stores.  The two-address commute
heuristics were measured the same way at 0.02–0.06% (in the
audio-mixing review), and the 64-bit select of zero only concerns the
32.32 mixer loop.

What follows, by value:
1. **Tail calls** ("tail-call optimization — measured opportunity") —
   4–5% of calls already sit in tail position, but weighted by
   execution that is 0.649% of kernel instructions and 0.022% of
   Doom's.  Nine tenths of the kernel's free-frame win is
   `turnstile_exit` and `kpreempt_disabled`, both NetBSD base and so
   reachable only from the compiler; `VOP_*` dispatch is disqualified
   outright, passing the address of its own frame.  15–18 KB of kernel
   text once wrappers lose their frames.  A first cut without outgoing
   stack arguments is small.
2. **Inlined small memory calls and the `p - K` and MMIO folds** —
   broad in the kernel but unmeasured at run time; the kernel
   `memmove`'s backward byte loop is 10 instructions where 6 do.
3. **The rest** — at most 0.1% each; the zero-low-half carry and the
   64-bit select of zero go with the 32.32 mixer.

Copies are 10.5–15% of all instructions (kernel 12.8%, libc 15.0%,
Doom 10.5%).  With the SP and zero copies gone (`56c8b327d3af`,
`c6c5049b8f13`), what is left is argument and return marshalling and
PHI copies across blocks, which no scan has classified yet, plus a
handful of `LEAfi` expansions (a frame address used as a value: 10
in the kernel, 21 in libc, 3 in Doom, 16 in the tests).

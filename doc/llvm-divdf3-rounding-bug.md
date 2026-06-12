# `__divdf3` rounds 1 ULP short — investigation notes

**Status: RESOLVED 2026-06-11.** Surfaced by the first compiler-rt
rebuild since the recent codegen campaign. Tracked in `doc/TODO.md`
("Compiler: two scalar miscompiles surfaced by rebuilding
compiler-rt").

## Resolution

Root cause was **not** the rounding compare/select (and not
`8a23e2222632`, the prime suspect below — the rounding code was
correct and even clawed one ULP back). The pre-round quotient itself
was 2 ULPs short because `wideMultiply` lost a carry:

- In `fp_div_impl.inc` the low half of the 128-bit product (`dummy`)
  is dead, so the 32-bit sums of the `r1` partial-product row are
  dead — but their **carry-outs** still feed the high half
  (`hiWord(r1)`).
- The selector's carry-in fusion (`carryInAlreadyLive` in
  `PenumbraInstructionSelector.cpp`) selects a `G_UADDE` to a bare
  ADC reading SR.C and drops the use of the s1 carry vreg. Bottom-up
  selection then reaches the producer `G_UADDO`, finds all defs
  unused, and `InstructionSelect` erases it as trivially dead —
  deleting the flag-producing ADD and, transitively, the `plolo`
  multiply cone. The surviving ADCs read stale carries.
- Fix: reject fusion when the producer's sum register is dead
  (`MRI.use_nodbg_empty`); the fallback materializes the carry
  through a GPR, which keeps the producer selectable.

One fix cleared all three known symptom groups: the three -O2
`print_double` test failures, `__umodXi3` at -O0 (the G_USUBO/G_USUBE
flavor inside the -O0-compiled division loop), and the previously
unexplained `pr23135.c` -O0 failure. Full suite: 1613/1613 at -O2.
Regression test: `test/CodeGen/Penumbra/uaddo-dead-sum.ll` (verified
to fail against the unfixed selector).

Method note for future miscompile hunts: the localization that
cracked this was *trace diffing* — instrument a host build of the
same compiler-rt source with hex prints of every intermediate, run
the witness vector on the ISS with `+trace=`, and find the first
host intermediate value that never appears in the trace. That named
the dead expression (`hiWord(plolo)`) without a single
recompile-bisect step.

A separate bug found during the investigation (bare mulhi idiom →
i128 → legalizer ICE) is tracked in `doc/TODO.md` ("Compiler: G_ZEXT
s128 from the mulhi idiom fails to legalize").

---

Original investigation notes follow (the "Where to look" hypothesis
was disproven; kept for the record).

## Symptom

compiler-rt's `__divdf3`, compiled by the current in-tree clang,
returns a result one ULP below the correctly rounded quotient for at
least some divisions whose exact quotient is representable.

**Proven bad vector** (single operation, exact inputs):

```
__divdf3(0x4341c37937e08000 /* 1e16 */, 0x4024000000000000 /* 10.0 */)
  returns  0x430c6bf52633ffff
  expected 0x430c6bf526340000   /* exact 1e15 */
```

Walking a power-of-ten chain down from 1e18 (`t /= 10.0` repeatedly)
shows the first two steps exact and everything after short:

| step | result (ISS)        | correct             |
|------|---------------------|---------------------|
| 1e18/10 | `4376345785d8a000` ✓ | same             |
| /10  | `4341c37937e08000` ✓ | same                |
| /10  | `430c6bf52633ffff`   | `430c6bf526340000` |
| /10  | `42d6bcc41e8fffff`   | `42d6bcc41e900000` |
| /10  | `42a2309ce53fffff`   | `42a2309ce5400000` |
| /10  | `426d1a94a1fffffd`   | `426d1a94a2000000` |

(Steps 4+ feed the corrupted previous result back in, so only step 3
is a clean single-op witness; treat the rest as chain behavior.)

The wrong results are identical when `divdf3.c` is compiled at `-O2`
and at `-O3`.

## How it manifests in the test suite

Three `make test-compiler` failures: `Regression/C/casts.c`,
`UnitTests/2005-05-12-Int64ToFP.c`, `UnitTests/2005-07-17-INT-To-FP.c`.
All go through the harness's `print_double`
(`test/compiler/harness/libc_stub.c`), whose integer-digit loop
maintains `top /= 10.0` per digit and computes
`digit = (int)(d / top); d -= digit * top`. With `top` one ULP short
the residue drifts: formatting `(double)1152921504606847100LL` leaves
a final residue of `0x3fd8480984872340` (≈ 0.3794) where the host
computes exactly `0.0` — visible as junk fractions
(`…976.379396`, `…808.578872`) and, when the drift crosses a digit
boundary, off-by-one integer digits (`…953` for `…952`).

Per-iteration comparison against host-Python double arithmetic shows
the divergence starting exactly at the iteration whose `top` is the
first wrong chain value, and every printed digit before that exact.

## What is ruled out (don't re-litigate these)

- **The aggregate-ABI rework (PenumbraABIInfo).** `clang -emit-llvm`
  of `divdf3.c` contains zero aggregate constructs — no `byval`, no
  `sret`, no struct/array types; the signature is
  `double @__divdf3(double, double)` plus `llvm.ctlz.i64` /
  `llvm.fabs.f64` declares. The classifier never executes for this
  code.
- **Optimization level of the runtime.** Standalone `divdf3.c.o`
  built at `-O2` and `-O3` produce identical wrong bits.
- **The sibling helpers**, unit-tested bit-exact on the relevant
  vectors: `__muldf3` (incl. the ascending `t *= 10.0` chain from 1.0
  to exact 1e18), `__subdf3`, `__floatundidf`, `__fixunsdfdi`,
  `__fixdfsi`, `__floatdidf`, and the integer division family
  (`__udivdi3`, `__umoddi3`, `__udivmoddi4` with both register- and
  stack-positioned arguments, operands above 2^32 included).
- **Callee-saved register clobbering in the helpers.** Prologue
  audit of `muldf3.c.o` / `floatsidf.c.o`: the set of R5–R10 written
  equals the set saved.
- **`va_arg(double)` transport.** All five variadic slot positions
  (register pair, straddle, full stack) deliver correct doubles.

## Where to look

`llvm/compiler-rt/lib/builtins/divdf3.c` →
`fp_div_impl.inc`. The final rounding step adjusts the quotient by
comparing a residual against the denominator — a compare-and-select
(or compare-and-increment) over `u64` halves. A 1-ULP-short result is
precisely "the round-up adjustment was not taken", which points at
miscompiled `i64` compare/select sequences rather than at the
Newton–Raphson reciprocal (a wrong reciprocal would not produce
consistently-one-ULP-short results on exact quotients).

**Prime suspect:** `8a23e2222632` ("llvm: fold constant compare
operands into select pseudos") — recent, and exactly the shape of the
rounding code. The regression window is everything between the
previous archive build (weeks old; predates the codegen campaign) and
HEAD.

## Repro recipe

```sh
cat > /tmp/divten.c <<'EOF'
extern int putchar(int);
typedef unsigned long long u64;
typedef union { double d; u64 u; } B;
extern double __divdf3(double, double);
static void puthex(u64 v) {
  int j;
  for (j = 60; j >= 0; j -= 4) {
    int n = (v >> j) & 0xF;
    putchar(n < 10 ? '0' + n : 'a' + n - 10);
  }
  putchar('\n');
}
volatile u64 e18 = 0x43abc16d674ec800ULL;
int main(void) {
  B t; t.u = e18;
  int i;
  for (i = 0; i < 6; i++) {
    t.d = __divdf3(t.d, 10.0);
    puthex(t.u);
  }
  return 0;
}
EOF
make test-compiler COMPILER_TESTS=/tmp/divten.c
D=$(ls -dt build/test-compiler/*divten* | head -1)
sw/sim/penumbra-iss $D/test.hex +hosted +quiet
# host reference:
python3 -c "
import struct
t = 1e18
for _ in range(6):
    t /= 10.0
    print(format(struct.unpack('<Q', struct.pack('<d', t))[0], '016x'))"
```

**Harness gotcha:** ad-hoc tests have no `.reference_output`, so the
runner judges them by exit code alone — a test like the above
"passes" while printing wrong bits. Compare output by eye or
`abort()` on mismatch.

To pin the suspect without the harness: compile `divdf3.c` standalone
with the runtime flags and link it in front of the archive (explicit
objects win over archive members):

```sh
CC="build/llvm/bin/clang --target=penumbra-unknown-none"
RTFLAGS="-ffreestanding -nostdinc -isystem $PWD/build/llvm/lib/clang/22/include -fno-builtin"
$CC $RTFLAGS -O2 -c llvm/compiler-rt/lib/builtins/divdf3.c -o /tmp/divdf3.o
# then link /tmp/divdf3.o before the builtins archive in the
# run-tests.py link line (crt0.o libc_stub.o test.o divdf3.o archive).
```

## Bisect recipe

Only clang needs rebuilding per step (the harness compiles tests with
clang, not llc):

```sh
git bisect start HEAD <last-good>     # last-good: previous archive build era
# per step:
ninja -C build/llvm -j10 clang
$CC $RTFLAGS -O2 -c llvm/compiler-rt/lib/builtins/divdf3.c -o /tmp/divdf3.o
# link + run divten as above; bad = third line ends ...2633ffff
```

A faster first probe than bisecting: revert `8a23e2222632` alone and
re-run the repro.

## Related: `__umodXi3` wrong results at -O0

Found in the same rebuild, distinct symptom: with the runtime built at
`-O0` (`sw/tools/setup-compiler-rt.sh -O0`, linked by
`make test-compiler OPT=-O0`), `int_div_impl.inc`'s `__umodXi3`
computes wrong remainders outright — `100 % 10` nonzero; every decimal
`printf` in the harness emits garbage (`100` printed as `15 `). The
implicit `v % d; v /= d` sequence misbehaves while a direct
`__udivmoddi4(v, d, &rem)` call from a `-O2` caller is correct, so the
wrongness is inside the -O0-compiled division loop, not in the call
transport. Not yet localized further. Check whether the `__divdf3` fix
clears this too before opening a second investigation — both shapes
are compare+branch/select loops over u64, and the known `-O0`-only
failure class (`pr23135.c`, see `test/compiler/excludes.txt`) may be
the same root cause.

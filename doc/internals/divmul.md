# Penumbra CPU — MUL/DIV Unit (divmul)

## Overview

The divmul unit implements hardware multiply and divide for the
Penumbra ISA:

| Instruction | Operation                                                                 | Cycles (target) |
|-------------|---------------------------------------------------------------------------|:---------------:|
| MUL, MULU   | `Rdh:Rd = Rd × Rs` (signed/unsigned 32×32→64; `Rdh=R0` → low half only)   | ~34             |
| DIV, DIVU   | `Rd, Rdh = (Rdh:Rd)/Rs, (Rdh:Rd)%Rs` (`Rdh=R0` → plain 32/32; else 64/32) | ~34             |

The unified `DIV` opcode covers both the common 32/32 case (`Rdh=R0`,
high half reads as zero) and the narrowing 64/32 case (`Rdh` holds the
dividend high half). No separate `DIVL` opcode exists — the hardware
iterates identically; only the initial value of `accum_hi` differs, and
that comes naturally from reading the `Rdh`-named register. Same
pattern for `MUL`: when `Rdh=R0` the high half of the product is
silently dropped, when `Rdh` is a real register both halves are
captured.

The unit is a **peer to the ALU**, sitting alongside it on the
register-bus fabric. It uses the same micro-sequencer protocol pattern
the ALU would have used for a multi-cycle op (1-cycle start pulse →
busy assertion → unified STALL → writeback), but with its **own**
`divmul_start`/`divmul_busy`/`divmul_op` signals — there is no shared
ALU FSM, and the ALU itself is single-cycle only (see
[datapath.md](./datapath.md)). From the micro-sequencer's perspective
MUL/DIV look like any other busy-asserting operation; only the STALL
gate sees that there are multiple busy sources to OR together
(`cache_busy | divmul_busy | …`).

## External Interface

| Signal              | Direction      | Purpose                                                  |
|---------------------|----------------|----------------------------------------------------------|
| `i_a_bus[31:0]`     | CPU → divmul   | First operand: multiplicand / dividend low half           |
| `i_b_bus[31:0]`     | CPU → divmul   | Second operand: multiplier / divisor                      |
| `i_rdh_bus[31:0]`   | CPU → divmul   | Third operand: dividend high half (reads as 0 when Rdh=R0)|
| `i_op[1:0]`         | CPU → divmul   | MUL (00), MULU (01), DIV (10), DIVU (11)                  |
| `i_start`           | CPU → divmul   | 1-cycle pulse — latch operands, begin iteration           |
| `o_busy`            | divmul → CPU   | Held high during iteration; falling edge = done           |
| `o_fault`           | divmul → CPU   | DIV0 or quotient-overflow detected; raises `VEC_ARITH`    |
| `o_result_lo[31:0]` | divmul → R-bus | Low half (writeback cycle 1)                             |
| `o_result_hi[31:0]` | divmul → R-bus | High half (writeback cycle 2)                            |
| `o_n`, `o_z`        | divmul → SR    | Flag outputs (latched on `flag_w_en` in writeback)       |

Operand latching is single-cycle: the CPU drives `i_a_bus`, `i_b_bus`,
and `i_rdh_bus` simultaneously, pulses `i_start`, and the unit
captures all three. `i_rdh_bus` reads as zero when the assembler-named
Rdh is R0 — no special "ignore Rdh" signal is needed; the
register-file's R0 read port produces the right value automatically. There is no multi-cycle operand-load protocol
— the 32-bit-wide internal bus carries each operand on its own wire,
and the divmul's internal 64-bit accumulator is loaded from the
combination.

## Internal Datapath

```
          A-bus[31:0]      B-bus[31:0]    Rdh-bus[31:0]
              │                 │                │
              v                 v                v
        ┌──────────────────────────────────────────────┐
        │            Operand latches (i_start)         │
        └────┬─────────────┬──────────────┬────────────┘
             │             │              │
             v             v              v
      ┌────────────┐  ┌──────────┐  ┌────────────┐
      │  accum_lo  │  │  divisor │  │  accum_hi  │
      │  (32-bit   │  │ (32-bit  │  │  (32-bit   │
      │   shift    │  │  hold)   │  │   shift    │
      │   reg)     │  │          │  │   reg)     │
      └─────┬──────┘  └────┬─────┘  └─────┬──────┘
            │              │              │
            │       ┌──────┘              │
            │       │                     │
            │       v                     │
            │   ┌─────────────────────────┴────────┐
            │   │     32-bit adder/subtractor      │
            │   │  (B_eff = divisor ⊕ {32{sub}};   │
            │   │   Cin = sub)                     │
            │   └────────────────┬─────────────────┘
            │                    │
            │                    v
            └──── shift control ─┴── feedback into accum_hi
                                     (MUL: conditional add of divisor;
                                      DIV: conditional sub of divisor;
                                      both shift accum_hi:accum_lo left
                                      or right by 1 each iteration)

      ┌──────────┐
      │ iter_cnt │   6-bit down-counter, init = 32, drives o_busy
      └──────────┘

      ┌──────────────────────────────────────────┐
      │  Fault detect (combinational at start):  │
      │    DIV0    ← (op ∈ DIV/DIVU) && (B==0)   │
      │    OVF     ← (op ∈ DIV/DIVU) && (Rdh ≥ B)│
      │    o_fault ← DIV0 | OVF                  │
      └──────────────────────────────────────────┘

The OVF check covers the narrowing form: when `Rdh != 0` and
`Rdh >= divisor`, the 32-bit quotient cannot fit. When `Rdh == 0`
(the plain 32/32 case), OVF is always false. Both faults are **purely
defensive** — they indicate caller bugs (compiler emitting wrong code,
hand-written multi-precision routines violating their own invariant)
and trap to `VEC_ARITH` for `SIGFPE` delivery, never as part of a
normal algorithm's control flow.
```

`accum_hi:accum_lo` form a 64-bit logical shift register. The same
physical structure serves both multiply (low-to-high shift, product
accumulates into accum_hi) and divide (high-to-low shift, quotient
bits accumulate into accum_lo as the algorithm consumes dividend
bits).

## Initial Conditions per Operation

| Op    | accum_lo ← | accum_hi ← | divisor ← | iter ← |
|-------|------------|------------|-----------|--------|
| MUL   | A (multiplier)   | 0     | B (multiplicand) | 32 |
| MULU  | A                | 0     | B                | 32 |
| DIV   | A (dividend low) | Rdh   | B (divisor)      | 32 |
| DIVU  | A                | Rdh   | B                | 32 |

`accum_hi` always loads from the register named in the `Rdh` field —
no hardware mux for "zero vs Rdh". The 32/32 case (`Rdh = R0`) gets
the right behaviour for free because R0 reads as zero. `MUL`'s
`accum_hi` is forced to zero by the iteration setup, since multiply
doesn't consume a high half. The iteration loop is identical across
all four operations once `accum_hi` is initialized.

## Multiplication — shift-add (unsigned)

```
for iter in 32:
    if accum_lo[0] == 1:
        accum_hi ← accum_hi + divisor    ; ("divisor" holds multiplicand)
    {accum_hi, accum_lo} ← {accum_hi, accum_lo} >> 1
```

After 32 iterations `accum_hi:accum_lo` holds the full 64-bit product.

For **signed** MUL: simplest correct implementation is sign-magnitude
— negate both operands at the start if negative, multiply, negate the
product if signs differed. Two extra cycles. (Booth's algorithm
eliminates the negate cycles but adds gates; skip it for the first
implementation.)

## Division — non-restoring (unsigned)

```
for iter in 32:
    {accum_hi, accum_lo} ← {accum_hi, accum_lo} << 1
    if accum_hi[31] == 0:                 ; previous step did not borrow
        accum_hi ← accum_hi - divisor
    else:
        accum_hi ← accum_hi + divisor

    accum_lo[0] ← ~accum_hi[31]           ; quotient bit
final_correction:
    if accum_hi[31] == 1:                 ; last step over-subtracted
        accum_hi ← accum_hi + divisor
```

After 32 iterations + final correction, `accum_lo` is the quotient and
`accum_hi` is the remainder.

Non-restoring saves the per-iteration register write that restoring
division does on every failed subtraction — same iteration count,
fewer gates in discrete (~3 chips saved vs restoring).

For **signed** DIV: convert dividend and divisor to absolute values,
divide unsigned, then apply C99 sign rules (quotient sign = XOR of
operand signs; remainder sign = dividend sign).

## Microcode Sequences

The micro-routine entry points sit at the same offsets the
illegal-instruction handler currently occupies. Each is 4 micro-ops.

**MUL / MULU / DIV / DIVU** all share the same 4-µ-op shape; the
divmul unit's behaviour differs internally based on `divmul_op` and
the `Rdh` operand, but the micro-routine is identical:

```
op-0:  reg_a_sel=Rd, reg_b_sel=Rs, b_mux=register, divmul_op=MUL|DIV|…,
       divmul_start=1, branch_cond=SEQ
op-1:  branch_cond=STALL              ; wait for divmul_busy fall (also catches o_fault)
op-2:  divmul_hi_drive=0 → accum_lo onto R-bus,
       reg_w_sel=Rd, reg_w_en=1, flag_w_en=1, branch_cond=SEQ
op-3:  divmul_hi_drive=1 → accum_hi onto R-bus,
       reg_w_sel=IR[15:12] (Rdh), reg_w_en=1, branch_cond=FETCH (pc_src=PC+4)
```

The fault is detected combinationally at start; if `o_fault` asserts,
the STALL resolution at op-1 triggers `VEC_ARITH` instead of
proceeding to writeback.

The third-operand read (`Rdh` → `i_rdh_bus`) happens at op-0
unconditionally — both MUL and DIV read `Rdh` from the register file
on the start cycle. For MUL it's ignored (the multiplier doesn't need
a dividend high half); for DIV it loads `accum_hi`. R0 reading as zero
gives the right behaviour for the 32/32 case automatically.

The writeback at op-3 sends `accum_hi` to the register named in
`IR[15:12]`. When that's `R0` (the common 2-operand case), the
register file silently drops the write — no special-case handling
needed, no microcode branching to "skip writeback if Rdh=R0".

Three new micro-word fields are needed:
- `divmul_start` (1 bit) — pulse to latch operands and begin iteration
- `divmul_op` (2 bits) — selects MUL (00) / MULU (01) / DIV (10) / DIVU (11)
  (alternative: omit and decode from `IR[26:25]`, since divmul only
  ever starts in response to a MUL/DIV opcode — saves 2 µ-word bits at
  the cost of a small decode in the unit)
- `divmul_hi_drive` (1 bit) — selects accum_hi (vs accum_lo) onto R-bus
  during writeback

The old 2-bit micro-word spare was consumed by the audit that added
`priv`/`ei_set`/`di_set` (51-bit micro-word). Divmul therefore extends
the micro-word — by 4 bits if `divmul_op` lives in the µ-word, or 2
bits if it's IR-decoded. Either variant fits in the 7-byte/56-bit
discrete-ROM slot without adding a chip. Final bit layout pinned
during RTL implementation (see `microcode.md`).

Total microcode footprint: 4 ROM entries × 4 opcodes = 16 entries,
replacing the 4 illegal-instruction-trap entries currently there.
Dispatch slots are `0x40` (MUL), `0x42` (MULU), `0x44` (DIV),
`0x46` (DIVU) — in the µROM's SYS-half region.

## Arithmetic Faults — `VEC_ARITH`

`o_fault` is combinational from the start cycle: the unit knows
immediately whether to fault. The CPU's STALL resolution gets a new
input alongside the existing memory-fault path:

| Condition                          | Resolution                                  |
|------------------------------------|---------------------------------------------|
| `divmul_busy=1`                    | Hold micro-PC                               |
| `divmul_busy=0`, `o_fault=0`       | micro-PC++ (normal)                         |
| `divmul_busy=0`, `o_fault=1`       | Trigger exception, `vector_num = VEC_ARITH` |

EPC points at the trapping `DIV` instruction (since `pc_src=001`
hasn't executed). The kernel handler delivers `SIGFPE` and leaves
EPC alone, so a userland `SIGFPE` handler that wants to retry can.

**Defensive only.** Both fault paths (DIV0 and narrowing-DIV quotient
overflow) indicate caller bugs — compiler emitting wrong code, or
hand-written multi-precision routines violating their own invariant.
Correct code, including compiler-rt's `__udivdi3` slow path with
Knuth Algorithm D, *verifies preconditions in software* before issuing
each DIV; the hardware fault is a safety net, not a normal control
signal. Consequently the implementation has no requirement to deliver
the fault quickly (e.g., as branch input to subsequent code) — it only
needs to stop the iteration immediately and signal before the next
instruction commits.

The fault priority ordering on STALL resolution becomes:
bus_fault (0) > align (8) > tlb_prot (3) > tlb_miss (2) > arith (10).
Memory faults strictly outrank arithmetic faults because they indicate
a more fundamental problem (no device responded, misalignment).

## Flag Generation

| Op family    | Z              | N        | C | V |
|--------------|----------------|----------|---|---|
| MUL, MULU    | Rd == 0        | Rd[31]   | 0 | 0 |
| DIV, DIVU    | Rd == 0 (quotient) | Rd[31] | 0 | 0 |

Z and N reflect the **low half / quotient** (Rd value), not the full
64-bit result. This matches user expectation for the common 32-bit
multiply (`BEQ` after `MUL R1, R2` tests whether the low 32 bits are
zero, just like every other ALU op) and for divide (test whether
quotient is zero).

Users who need to test the high half of a multiply can `CMP Rdh, R0`
after the multiply — one extra instruction, but the common case is
unaffected.

## Software Composition — `__udivdi3` worked example

The C operation `uint64_t / uint64_t` always lowers to a libcall to
compiler-rt's `__udivdi3`. With the unified `DIV` (which covers both
32/32 and 64/32 via its optional `Rdh` operand), the fast path is
**branchy** — most calls dispatch into a single 32/32 divide or a
single 64/32 narrowing divide; the dual-divide path is reached only
when the quotient genuinely doesn't fit in 32 bits.

### Algorithm (fast path, `d.hi == 0`)

```
uint64_t __udivdi3(uint64_t n, uint64_t d) {
    uint32_t n_lo = n;            uint32_t n_hi = n >> 32;
    uint32_t d_lo = d;            uint32_t d_hi = d >> 32;

    if (d_hi != 0)
        return __udivdi3_slow(n, d);  // Knuth Algorithm D, out of scope

    if (d_lo == 0) trap_div0();       // hardware would trap anyway

    if (n_hi == 0)
        // Plain 32/32 — one DIV with Rdh=R0 (or omitted entirely)
        return n_lo / d_lo;

    if (n_hi < d_lo) {
        // Quotient fits in 32 bits → one narrowing DIV (Rdh=n_hi as input)
        uint32_t q_lo, r;
        DIV(q_lo, r, /*Rd=*/n_lo, /*Rs=*/d_lo, /*Rdh in=*/n_hi);
        return q_lo;
    }

    // n_hi >= d_lo: quotient genuinely exceeds 32 bits → two divides.
    // Step 1: compute high half of quotient with a plain DIV.
    uint32_t q_hi, r1;
    DIV(q_hi, r1, /*Rd=*/n_hi, /*Rs=*/d_lo);  // plain 32/32

    // Step 2: compute low half with a narrowing DIV; r1 < d_lo is
    // guaranteed by step 1, so the narrowing form's precondition holds.
    uint32_t q_lo, r2;
    DIV(q_lo, r2, /*Rd=*/n_lo, /*Rs=*/d_lo, /*Rdh in=*/r1);

    return ((uint64_t)q_hi << 32) | q_lo;
}
```

### Penumbra assembly (fast path)

The Penumbra ABI passes 64-bit values in aligned register pairs (low,
high), so the dividend lands in `R1:R2` (`R1`=`n_lo`, `R2`=`n_hi`) and
the divisor in `R3:R4`. The return value pair is also `R1:R2`. Leaf
function — no prologue, no frame, no `R13` spill.

```
__udivdi3:
        CMP   R4, R0              ; d_hi == 0?
        BNE   .Lslow              ; no → Knuth Algorithm D

        CMP   R2, R0              ; n_hi == 0?
        BEQ   .Lplain_32          ; yes → plain 32/32 divide

        CMP   R2, R3              ; n_hi vs d_lo (unsigned)
        BHS   .Ldual              ; n_hi >= d_lo → dual-divide path

        ; n_hi < d_lo: quotient fits in 32 bits, single narrowing DIV.
        ;   R1 = n_lo, R2 = n_hi (dividend hi), R3 = d_lo
        DIV   R1, R3, R2          ; R1 = (R2:R1)/R3, R2 = remainder
        MOV   R2, R0              ; quotient hi = 0 (return convention)
        RET

.Lplain_32:
        ; n_hi == 0: simplest case, plain 32/32.
        ;   R1 = n_lo, R3 = d_lo, R2 already 0
        DIV   R1, R3              ; R1 = R1/R3, remainder discarded
        RET                       ; R2 stays 0 from the caller's view

.Ldual:
        ; n_hi >= d_lo: need both halves of quotient.
        ;   R1 = n_lo, R2 = n_hi, R3 = d_lo
        DIV   R2, R3, R5          ; R2 = n_hi/d_lo (q_hi), R5 = n_hi%d_lo (r1)
        DIV   R1, R3, R5          ; R1 = (R5:R1)/d_lo (q_lo), R5 = remainder
        RET                       ; returns R1:R2 = q_lo:q_hi

.Lslow:
        ; Knuth Algorithm D — verifies each digit-estimate's invariant
        ; in software before issuing the narrowing DIV. Out of scope.
        ...
```

The three fast-path exits are each a single divide (~34 cycles) plus
a handful of compares/branches/moves — call it ~40 cycles total. Only
the dual-divide path (rarely reached in real code) pays ~70 cycles.
Pure-software shift-subtract emulation on 64-bit operands costs
roughly 2000 cycles in the same units; the typical real-world speedup
is therefore ~50× rather than the 30× a uniform two-divide path would
deliver.

### Why the dual-divide path runs `DIV` twice in that order

<!-- TODO(human): write 2–3 sentences explaining why the `.Ldual` path
must execute the plain `DIV` (32/32) on the high half *before* the
narrowing `DIV` on the low half — i.e., what invariant the first
`DIV`'s remainder establishes that the second `DIV`'s precondition
requires.

Hints:
- DIV's remainder semantics: after `DIV R2, R3, R5`, `R5 = R2_original
  % R3`. By the definition of integer division, what bound does R5
  have relative to R3?
- The narrowing-DIV precondition (instruction-set.md § Narrowing-DIV):
  the dividend high half (input `Rdh`) must be strictly less than the
  divisor for the 32-bit quotient to fit.
- The two constraints meet exactly — the first DIV's remainder is
  *constructed* to be a legal high-half input for the second DIV.
  This is the same invariant schoolbook long division relies on for
  "carry down": each digit's partial remainder is bounded below the
  divisor, so the next digit's quotient always fits.

Keep it 2–3 sentences. The point is to make the dependency between
the two hardware instructions explicit so a reader knows the order
is forced, not arbitrary.
-->

## Discrete Chip-Count Estimate

| Block                               | Implementation                         | Chips |
|-------------------------------------|----------------------------------------|:-----:|
| `accum_lo` 32-bit shift register    | 4× 74HC299 (8-bit universal shift)     | 4     |
| `accum_hi` 32-bit shift register    | 4× 74HC299                             | 4     |
| `divisor` 32-bit hold register      | 4× 74HC374                             | 4     |
| 32-bit adder/subtractor             | 8× 74HC283 (4-bit CLA)                 | 8     |
| Sub-mode B-invert + Cin gating      | 1× 74HC86 + 1× 74HC00                  | 2     |
| `iter_cnt` 6-bit down-counter       | 2× 74HC163                             | 2     |
| Conditional add/sub gating (mul: q-bit; div: sign of accum_hi) | discrete gates    | 2     |
| Fault detect (B==0, Rdh ≥ B compare)| 1× 74HC85 + a couple of gates          | 2     |
| Local sequencer (init / iter / writeback) | 1× GAL22V10                      | 1     |
| Output mux to R-bus (lo/hi select)  | 4× 74HC157                             | 4     |

**Total: ~33 chips.** Roughly the same complexity as the existing main
ALU. The shared shift/add structure is the key economy — separating
multiply and divide units would roughly double this count.

If you're willing to share the adder with the main ALU (multiplexing
on a per-cycle basis), the count drops to ~25, but adds an adder
arbitration constraint that's annoying in microcode. Probably not
worth it for the first build.

## Implementation Status

- **ISA spec:** finalized (this document + `instruction-encoding.md`).
- **Microcode:** not yet written. The four divmul dispatch slots
  (0x40 MUL, 0x42 MULU, 0x44 DIV, 0x46 DIVU) currently route to the
  illegal-instruction handler.
- **RTL:** not yet written. The sequencer has an unused `alu_busy`
  input (tied off — the ALU is single-cycle); divmul implementation
  will add the peer unit and rename it to `divmul_busy`, plus add
  `divmul_start`/`divmul_op` outputs from the µ-word decoder.
- **Software fallback:** `librt.c` and compiler-rt provide working
  software emulation, which remains the runtime fallback until the
  hardware exists.

## See Also

- [`doc/system/instruction-set.md`](../system/instruction-set.md) — programmer-facing
  MUL/DIV reference, assembler syntax, register-pair convention, narrowing-DIV
  precondition
- [`doc/system/instruction-encoding.md`](../system/instruction-encoding.md) — Format R
  opcode partition, sub-encoding with `Rdh`
- [`doc/internals/datapath.md`](./datapath.md) — ALU vs peer-unit
  partitioning, micro-sequencer STALL behaviour
- [`doc/system/architecture.md`](../system/architecture.md) — `VEC_ARITH`
  in the exception vector table

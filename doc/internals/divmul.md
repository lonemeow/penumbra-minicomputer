# Penumbra CPU — MUL/DIV Unit (divmul)

## Overview

The divmul unit implements hardware multiply and divide for the
Penumbra ISA:

| Instruction | Operation                                              | Cycles (target) |
|-------------|--------------------------------------------------------|:---------------:|
| MUL, MULU   | `Rdh:Rd = Rd × Rs` (signed/unsigned 32×32→64)          | ~34             |
| DIV, DIVU   | `Rd = Rd / Rs; Rdh = Rd % Rs` (signed/unsigned 32/32)  | ~34             |
| DIVL, DIVLU | `Rd, Rdh = (Rdh:Rd)/Rs, (Rdh:Rd)%Rs` (narrowing 64/32) | ~34             |

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
| `i_a_bus[31:0]`     | CPU → divmul   | First operand: multiplicand / 32-bit dividend / DIVL low |
| `i_b_bus[31:0]`     | CPU → divmul   | Second operand: multiplier / divisor                     |
| `i_rdh_bus[31:0]`   | CPU → divmul   | Third operand: DIVL dividend high half                   |
| `i_op[2:0]`         | CPU → divmul   | MUL, MULU, DIV, DIVU, DIVL, DIVLU                        |
| `i_start`           | CPU → divmul   | 1-cycle pulse — latch operands, begin iteration          |
| `o_busy`            | divmul → CPU   | Held high during iteration; falling edge = done          |
| `o_fault`           | divmul → CPU   | DIV0 or DIVL overflow detected; raises `VEC_ARITH`       |
| `o_result_lo[31:0]` | divmul → R-bus | Low half (writeback cycle 1)                             |
| `o_result_hi[31:0]` | divmul → R-bus | High half (writeback cycle 2)                            |
| `o_n`, `o_z`        | divmul → SR    | Flag outputs (latched on `flag_w_en` in writeback)       |

Operand latching is single-cycle: the CPU drives `i_a_bus`, `i_b_bus`,
and (for DIVL) `i_rdh_bus` simultaneously, pulses `i_start`, and the
unit captures all three. There is no multi-cycle operand-load protocol
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
      │    DIV0    ← (op ∈ DIV family) && (B==0) │
      │    OVF     ← (op ∈ DIVL family) && (Rdh ≥ B) │
      │    o_fault ← DIV0 | OVF                  │
      └──────────────────────────────────────────┘
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
| MULU  | A          | 0          | B         | 32     |
| DIV   | A (dividend)     | 0     | B (divisor)      | 32 |
| DIVU  | A          | 0          | B         | 32     |
| DIVL  | A (dividend low) | Rdh (dividend high) | B (divisor) | 32 |
| DIVLU | A          | Rdh        | B         | 32     |

The **only** difference between DIV and DIVL at the hardware level is
the initial value of `accum_hi` — DIV zeros it; DIVL latches it from
the `Rdh` operand bus. The iteration loop is identical.

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

**MUL / MULU:**
```
mul-0: reg_a_sel=Rd, reg_b_sel=Rs, b_mux=register, divmul_op=MUL{U},
       divmul_start=1, branch_cond=SEQ
mul-1: branch_cond=STALL              ; wait for divmul_busy fall (also catches o_fault)
mul-2: divmul_hi_drive=0 → accum_lo onto R-bus,
       reg_w_sel=Rd, reg_w_en=1, flag_w_en=1, branch_cond=SEQ
mul-3: divmul_hi_drive=1 → accum_hi onto R-bus,
       reg_w_sel=IR[15:12] (Rdh), reg_w_en=1, branch_cond=FETCH (pc_src=PC+4)
```

**DIV / DIVU / DIVL / DIVLU:** identical structure — the unit is
configured by `divmul_op` and (for DIVL) the third-operand latch path
on mul-0. The fault is detected combinationally at start; if `o_fault`
asserts, the STALL resolution triggers `VEC_ARITH` instead of
proceeding to writeback.

Four new micro-word fields are needed:
- `divmul_start` (1 bit) — pulse to latch operands and begin iteration
- `divmul_op` (3 bits) — selects MUL / MULU / DIV / DIVU / DIVL / DIVLU
  (alternative: omit and decode from `IR[29:25]`, since divmul only
  ever starts in response to a MUL/DIV opcode — saves 3 µ-word bits at
  the cost of a small decode in the unit)
- `divmul_hi_drive` (1 bit) — selects accum_hi (vs accum_lo) onto R-bus
  during writeback
- `rdh_read_en` (1 bit) — latches `Rdh` into accum_hi on the start
  cycle (DIVL only)

The old 2-bit micro-word spare was consumed by the audit that added
`priv`/`ei_set`/`di_set` (51-bit micro-word). Divmul therefore extends
the micro-word — by 6 bits if `divmul_op` lives in the µ-word, or 3
bits if it's IR-decoded. The 7-byte/56-bit discrete-ROM slot has room
for the IR-decoded variant without adding a chip; the explicit-op
variant needs an eighth byte. Final bit layout pinned during RTL
implementation (see `microcode.md`).

Total microcode footprint: 4 ROM entries × 6 opcodes = 24 entries,
replacing the 6 illegal-instruction-trap entries currently there.

## Arithmetic Faults — `VEC_ARITH`

`o_fault` is combinational from the start cycle: the unit knows
immediately whether to fault. The CPU's STALL resolution gets a new
input alongside the existing memory-fault path:

| Condition                          | Resolution                                  |
|------------------------------------|---------------------------------------------|
| `divmul_busy=1`                    | Hold micro-PC                               |
| `divmul_busy=0`, `o_fault=0`       | micro-PC++ (normal)                         |
| `divmul_busy=0`, `o_fault=1`       | Trigger exception, `vector_num = VEC_ARITH` |

EPC points at the trapping `DIV`/`DIVL` instruction (since `pc_src=001`
hasn't executed). The kernel handler can advance EPC to skip the
instruction after delivering `SIGFPE`, or leave it pointing at the
trap for a user-mode `SIGFPE` handler to retry.

The fault priority ordering on STALL resolution becomes:
bus_fault (0) > align (8) > tlb_prot (3) > tlb_miss (2) > arith (10).
Memory faults strictly outrank arithmetic faults because they indicate
a more fundamental problem (no device responded, misalignment).

## Flag Generation

| Op family    | Z              | N        | C | V |
|--------------|----------------|----------|---|---|
| MUL, MULU    | Rd == 0        | Rd[31]   | 0 | 0 |
| DIV, DIVU, DIVL, DIVLU | Rd == 0 (quotient) | Rd[31] | 0 | 0 |
| MOD, MODU    | (same as DIV, since MOD = DIV with quotient → R0) | | | |

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
compiler-rt's `__udivdi3`. With `DIV` + `DIVL` available, the fast
path of that routine (divisor fits in 32 bits, ~95% of real calls)
collapses to just two hardware divides.

<!-- TODO(human): worked example of __udivdi3 lowering.

Fill in this section with:

1. A C-style pseudocode sketch of the __udivdi3 fast path: detect
   "divisor.hi == 0", then use DIV + DIVL to compute the full 64-bit
   quotient and 32-bit remainder in two hardware ops.

2. The equivalent Penumbra assembly. Register conventions: arguments
   in R1–R4 per the ABI (R1 = dividend low, R2 = dividend high,
   R3 = divisor low, R4 = divisor high). Result returned in R1:R2.
   Use R5, R6, etc. as scratch.

3. One or two sentences explaining *why* DIV is called first on the
   dividend high half — i.e., what invariant the second DIVL needs
   from the first DIV's remainder.

Keep it concise (~15-25 lines of asm + ~5 lines of prose). The
slow path (divisor >= 2^32) is out of scope here — note that it
exists and uses Knuth Algorithm D with DIVL as the digit estimator,
but don't expand it.
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
- **Microcode:** not yet written. Six entries currently dispatch to
  the illegal-instruction handler.
- **RTL:** not yet written. The sequencer has an unused `alu_busy`
  input (tied off — the ALU is single-cycle); divmul implementation
  will add the peer unit and rename it to `divmul_busy`, plus add
  `divmul_start`/`divmul_op` outputs from the µ-word decoder.
- **Software fallback:** `librt.c` and compiler-rt provide working
  software emulation, which remains the runtime fallback until the
  hardware exists.

## See Also

- [`doc/system/instruction-set.md`](../system/instruction-set.md) — programmer-facing
  MUL/DIV/DIVL reference, assembler syntax, register-pair convention
- [`doc/system/instruction-encoding.md`](../system/instruction-encoding.md) — Format R
  sub-encoding with `Rdh`
- [`doc/internals/datapath.md`](./datapath.md) — ALU vs peer-unit
  partitioning, micro-sequencer STALL behaviour
- [`doc/system/architecture.md`](../system/architecture.md) — `VEC_ARITH`
  in the exception vector table

# Penumbra CPU — MUL/DIV Unit (divmul)

## Overview

The divmul unit implements hardware multiply and divide for the
Penumbra ISA:

| Instruction | Operation                                                                | Cycles (target) |
|-------------|--------------------------------------------------------------------------|:---------------:|
| MUL, MULU   | `Rdh:Rd = Rd × Rs` (signed/unsigned 32×32→64; `Rdh=R0` → low half only)  | ~34             |
| DIV, DIVU   | `Rd, Rdh = Rd / Rs, Rd % Rs` (32/32, signed/unsigned; `Rdh=R0` → quotient only) | ~34     |

The third operand `Rdh` is always **write-only** — for `MUL`/`MULU` it
receives the high half of the product, and for `DIV`/`DIVU` it receives
the remainder. Writes to `R0` are silently dropped, which is how the
2-operand forms discard the high half for free. Divides are always
32/32; there is no 64/32 narrowing form (see the rationale in
[instruction-set.md](../system/instruction-set.md)).

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
| `i_a_bus[31:0]`     | CPU → divmul   | First operand: multiplier / dividend                      |
| `i_b_bus[31:0]`     | CPU → divmul   | Second operand: multiplicand / divisor                    |
| `i_op[1:0]`         | CPU → divmul   | MUL (00), MULU (01), DIV (10), DIVU (11)                  |
| `i_start`           | CPU → divmul   | 1-cycle pulse — latch operands, begin iteration           |
| `o_busy`            | divmul → CPU   | Held high during iteration; falling edge = done           |
| `o_fault`           | divmul → CPU   | DIV0 detected; raises `VEC_ARITH`                         |
| `o_result_lo[31:0]` | divmul → W-bus | Low half  (quotient / product low)                        |
| `o_result_hi[31:0]` | divmul → W-bus | High half (remainder / product high)                      |
| `o_n`, `o_z`        | divmul → SR    | Flag outputs (latched on `flag_w_en` in writeback)        |

Operand latching is single-cycle: the CPU drives `i_a_bus` and `i_b_bus`
simultaneously, pulses `i_start`, and the unit captures both. Two
operand reads are all the regfile (2R/1W) can supply in one cycle; the
third operand `Rdh` names only the *destination* for the high half and
is consumed by the writeback µ-op, not as a hardware input. Results
reach the register file through the W-bus mux (`wb_src`), alongside the
ALU and load-data sources.

## Internal Datapath

```
          A-bus[31:0]      B-bus[31:0]
              │                 │
              v                 v
        ┌─────────────────────────────────┐
        │     Operand latches (i_start)   │
        └────┬──────────────┬─────────────┘
             │              │
             v              v
      ┌────────────┐   ┌──────────┐         ┌────────────┐
      │  accum_lo  │   │  divisor │         │  accum_hi  │
      │  (32-bit   │   │ (32-bit  │         │  (32-bit   │
      │   shift    │   │  hold)   │         │   shift    │
      │   reg)     │   │          │         │   reg, ←0) │
      └─────┬──────┘   └────┬─────┘         └─────┬──────┘
            │               │                     │
            │        ┌──────┘                     │
            │        │                            │
            │        v                            │
            │    ┌─────────────────────────────┬──┴───────┐
            │    │   32-bit adder/subtractor   │          │
            │    │  (B_eff = divisor ⊕ {32{sub}}; Cin=sub)│
            │    └────────────────┬────────────────────────┘
            │                     │
            │                     v
            └──── shift control ──┴── feedback into accum_hi
                                      (MUL: conditional add of divisor;
                                       DIV: conditional sub of divisor;
                                       both shift accum_hi:accum_lo left
                                       or right by 1 each iteration)

      ┌──────────┐
      │ iter_cnt │   6-bit down-counter, init = 32, drives o_busy
      └──────────┘

      ┌──────────────────────────────────────────┐
      │  Fault detect (combinational at start):  │
      │    o_fault ← (op ∈ DIV/DIVU) && (B == 0) │
      └──────────────────────────────────────────┘

`accum_hi` starts at 0 unconditionally (multiply has no dividend high
half; divide is 32/32). The DIV0 fault is **purely defensive** — it
indicates a caller bug (compiler emitting wrong code, hand-written code
dividing by a zero register) and traps to `VEC_ARITH` for `SIGFPE`
delivery. It is never part of normal algorithm control flow.
```

`accum_hi:accum_lo` form a 64-bit logical shift register. The same
physical structure serves both multiply (low-to-high shift, product
accumulates into accum_hi) and divide (high-to-low shift, quotient
bits accumulate into accum_lo as the algorithm consumes dividend
bits).

## Initial Conditions per Operation

| Op    | accum_lo ←             | accum_hi ← | op_b ← (mcand / divisor) | iter ← |
|-------|------------------------|------------|--------------------------|--------|
| MUL   | \|A\| (multiplier mag) | 0          | \|B\| (multiplicand mag) | 32     |
| MULU  | A                      | 0          | B                        | 32     |
| DIV   | \|A\| (dividend mag)   | 0          | \|B\| (divisor mag)      | 32     |
| DIVU  | A (dividend)           | 0          | B (divisor)              | 32     |

`accum_hi` always starts at 0. Signed `MUL` and signed `DIV` iterate on
the *magnitudes* of their operands and apply the sign on the output
side. The iteration loop is identical across all four operations once
the registers are initialized.

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

For **signed** DIV: iterate on |dividend|, |divisor|, then apply C99
sign rules, negating quotient and remainder *independently* (they are
two separate numbers, not halves of one): quotient sign = XOR of operand
signs, remainder sign = dividend sign.

`INT_MIN / -1` (signed overflow) is C undefined behaviour; the unit
returns `INT_MIN` (the unsigned engine yields `0x80000000`, which the
sign rules leave unnegated) and does not fault.

## Microcode Sequences

Each of MUL/MULU/DIV/DIVU is a 3-µop routine. The ×2 dispatch spacing
gives each opcode two ROM slots; the third µop is a shared high-half
writeback tail reached via `SKIP`. The dispatch slots are `0x40` (MUL),
`0x42` (MULU), `0x44` (DIV), `0x46` (DIVU) — at the top of the µROM's
op[4]=1 region. The shared tail sits at `0x49` (op20's second slot,
never a dispatch target).

```
op-0:  reg_a=IR_RD reg_b=IR_RS alu=<op> divmul_start=1 STALL
       ; latches Rd/Rs into divmul, waits for ~33-cycle iteration.
       ; o_fault is checked at the STALL: if it asserts, the sequencer
       ; aborts to VEC_ARITH instead of advancing.

op-1:  reg_w=IR_RD w_en=1 wb_src=DML_LO w_flags=1  SKIP→0x49
       ; writes the low half (quotient / product low) to Rd, latches Z/N.

tail:  reg_w=IR_RDH w_en=1 wb_src=DML_HI
       ; writes the high half (remainder / product high) to Rdh.
       ; If Rdh=R0 (2-operand form), the regfile silently drops the
       ; write — no microcode branch needed.
```

The divmul op is decoded from `i_alu_op` (which the sequencer already
emits for ALU ops); no separate `divmul_op` micro-word field is needed.
The micro-word grew from 51 to 52 bits to add `wb_src` (2 bits,
RBUS/MDR/DML_LO/DML_HI) in place of the old 1-bit `wmux`; `divmul_start`
reused the old `alu_start` field bit. Total microcode footprint: 4
dispatch slots × 2 entries + 1 shared tail = 9 ROM entries.

## Arithmetic Faults — `VEC_ARITH`

`o_fault` is combinational from the start cycle: the unit knows
immediately whether to fault (divisor is zero). A faulting divide does
not iterate; the sequencer aborts to `VEC_ARITH` on the same cycle.

| Condition                          | Resolution                                  |
|------------------------------------|---------------------------------------------|
| `divmul_busy=1`                    | Hold micro-PC                               |
| `divmul_busy=0`, `o_fault=0`       | micro-PC++ (normal)                         |
| `divmul_busy=0`, `o_fault=1`       | Trigger exception, `vector_num = VEC_ARITH` |

EPC points at the trapping `DIV` instruction. The kernel handler
delivers `SIGFPE` and leaves EPC alone, so a userland `SIGFPE` handler
that wants to retry can.

**Defensive only.** DIV0 indicates a caller bug — compiler emitting
wrong code, or hand-written code dividing by a zero register. The
hardware fault is a safety net, not a normal control signal. The
implementation has no requirement to deliver the fault quickly — it
only needs to stop the iteration immediately and signal before the next
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

## Software Composition

### 32-bit divide — `__udivsi3` becomes a single instruction

The C operation `uint32_t / uint32_t` lowers to one hardware `DIVU`,
zero libcall overhead. With the third operand, the compiler can fold
`uint32_t q = a / b; uint32_t r = a % b;` (or LLVM's `G_UDIVREM` /
`G_SDIVREM` fusion) into one instruction:

```
DIVU  R1, R2, R3   ; R1 = R1/R2 (quotient), R3 = R1%R2 (remainder)
```

The same shape applies to signed `DIV`, `MUL` (low only), and `MUL Rd,
Rs, Rdh` (full 64-bit product). The backend lowers `G_MUL`/`G_UDIV`/
`G_SDIV`/`G_UREM`/`G_SREM`/`G_UDIVREM`/`G_SDIVREM` at s32 directly to
these instructions.

### 64-bit divide — software, with one hardware primitive inside

`uint64_t / uint64_t` lowers to compiler-rt's `__udivdi3`, which is
software (the divmul peer is a 32-bit unit). The standard
shift-subtract long-division algorithm uses the hardware `DIVU` as its
inner primitive when one operand fits in 32 bits, and falls back to
bit-by-bit shifting for the genuinely-64-bit case. Signed
`__divdi3` is sign-magnitude over `__udivdi3`.

The 68000-style 64/32 narrowing divide was considered and rejected:
modern small RISCs (MIPS, ARM, RISC-V, SH) all decline to ship it
because 32×32→64 multiply makes long-division composable from the
32/32 primitive, and the hardware cost of a dividend-high *input*
(third regfile read port, prep micro-op, or dispatch-time hardware
trick) outweighs the benefit on the narrowing branch alone.

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
| Fault detect (B==0 compare)         | 1× 74HC30 (8-input NOR) + a gate       | 1     |
| Local sequencer (init / iter / writeback) | 1× GAL22V10                      | 1     |
| Output mux to R-bus (lo/hi select)  | 4× 74HC157                             | 4     |

**Total: ~32 chips.** Roughly the same complexity as the existing main
ALU. The shared shift/add structure is the key economy — separating
multiply and divide units would roughly double this count.

If you're willing to share the adder with the main ALU (multiplexing
on a per-cycle basis), the count drops to ~25, but adds an adder
arbitration constraint that's annoying in microcode. Probably not
worth it for the first build.

## Implementation Status

- **ISA spec:** finalized (this document + `instruction-encoding.md` +
  `instruction-set.md`).
- **Microcode:** implemented. Dispatch slots 0x40 (MUL), 0x42 (MULU),
  0x44 (DIV), 0x46 (DIVU) hold the 3-µop routines; the shared high-half
  writeback tail is at 0x49.
- **RTL:** implemented (`hw/rtl/core/divmul.sv`). The sequencer drives
  the peer unit via `divmul_start` (reusing the old `alu_start` field
  bit), stalls on `i_divmul_busy`, and aborts on `o_fault → VEC_ARITH`.
  Results reach the register file via the `wb_src` writeback-source mux
  (`DML_LO`/`DML_HI`), not through the R-bus.
- **ISS:** implements MUL/MULU/DIV/DIVU end-to-end with both halves and
  `VEC_ARITH` (`sw/sim/penumbra_iss.cpp`).
- **LLVM backend:** open work (`doc/TODO.md`). Today every 32-bit MUL/
  DIV/REM still libcalls — when the backend lowers `G_MUL`/`G_*DIV`/
  `G_*REM`/`G_*DIVREM` at s32 directly to the new instructions, real C
  code starts using the hardware unit.

## See Also

- [`doc/system/instruction-set.md`](../system/instruction-set.md) — programmer-facing
  MUL/DIV reference, assembler syntax, register-pair convention, DIV0 trap
- [`doc/system/instruction-encoding.md`](../system/instruction-encoding.md) — Format R
  opcode partition, sub-encoding with `Rdh`
- [`doc/internals/datapath.md`](./datapath.md) — ALU vs peer-unit
  partitioning, micro-sequencer STALL behaviour
- [`doc/system/architecture.md`](../system/architecture.md) — `VEC_ARITH`
  in the exception vector table

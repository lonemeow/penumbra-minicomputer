# Penumbra CPU — MUL/DIV Unit (divmul)

> **Applies to:** all generations · shared peer unit. The algorithm,
> handshake, and internal datapath are identical on both cores; *how a
> core drives the unit* — microcode on Penumbra/1, an EX-stage start
> plus a sequenced two-write commit on Penumbra/2 — lives in that
> core's docs.

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

The unit is a **peer to the ALU**, not part of it: a separate block on
the register-bus fabric with its own operand latches and FSM, driven
by a 1-cycle `start` pulse and reporting a `busy` level (the ALU
itself is single-cycle and never asserts busy). The CPU's control
logic latches operands, pulses `start`, then holds until `busy` falls
— looking, from the control side, like any other busy-asserting
multi-cycle operation that joins the unified busy/STALL gate
(`cache_busy | divmul_busy | …`). How each generation expresses that
driving differs — see [Driving the Unit](#driving-the-unit-per-generation).

## External Interface

| Signal              | Direction      | Purpose                                                  |
|---------------------|----------------|----------------------------------------------------------|
| `i_a[31:0]`         | CPU → divmul   | First operand: multiplier / dividend                      |
| `i_b[31:0]`         | CPU → divmul   | Second operand: multiplicand / divisor                    |
| `i_op[4:0]`         | CPU → divmul   | Operation select, in the core-internal ALU-field encoding: MUL `01101`, MULU `01110`, DIV `01111`, DIVU `10000` |
| `i_start`           | CPU → divmul   | 1-cycle pulse — latch operands, begin iteration           |
| `o_busy`            | divmul → CPU   | Held high during iteration; falling edge = done           |
| `o_fault`           | divmul → CPU   | DIV0 detected; raises `VEC_ARITH`                         |
| `o_result_lo[31:0]` | divmul → W-bus | Low half  (quotient / product low)                        |
| `o_result_hi[31:0]` | divmul → W-bus | High half (remainder / product high)                      |
| `o_flag_n`, `o_flag_z` | divmul → SR | Flag outputs (latched on `flag_w_en` in writeback)        |

The op encoding is the gen1 microcode ALU field, carried directly by
the micro-word; gen2's EX stage decodes its 2-bit ISA sub-opcode
selector into the same 5-bit codes before driving the unit.

Operand latching is single-cycle: the CPU drives `i_a` and `i_b`
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

## Division — restoring with borrow select (unsigned)

```
for iter in 32:
    {accum_hi, accum_lo} ← {accum_hi, accum_lo} << 1
    diff[32:0] ← {1'b0, accum_hi} - {1'b0, divisor}
    if diff[32] == 0:                     ; no borrow — divisor fits
        accum_hi ← diff[31:0]
        accum_lo[0] ← 1                   ; quotient bit
    else:                                 ; borrow — keep pre-subtract value
        accum_lo[0] ← 0
```

After 32 iterations, `accum_lo` is the quotient and `accum_hi` is the
remainder — there is no final correction step.

The "restore" costs nothing here: the subtractor's result and the
pre-subtract value are both present, and the 33-bit borrow simply
selects which one is registered — one mux, no extra cycle, no second
adder pass. (Classic non-restoring division avoids the restore by
alternating add/subtract steps plus a final correction; with the
borrow-select formulation the restoring form needs neither.)

For **signed** DIV: iterate on |dividend|, |divisor|, then apply C99
sign rules, negating quotient and remainder *independently* (they are
two separate numbers, not halves of one): quotient sign = XOR of operand
signs, remainder sign = dividend sign.

`INT_MIN / -1` (signed overflow) is C undefined behaviour; the unit
returns `INT_MIN` (the unsigned engine yields `0x80000000`, which the
sign rules leave unnegated) and does not fault.

## Driving the Unit (per generation)

How a core sequences `start`, the busy-wait, and the two-result commit
is generation-specific and lives in that core's docs:

- **Penumbra/1** — a 3-µop microcode routine per opcode (dispatch slots
  `0x40`–`0x46` with a shared high-half writeback tail). See the
  *MUL/DIV dispatch routines* in
  [`penumbra1/microcode.md`](./penumbra1/microcode.md#muldiv-dispatch-routines).
- **Penumbra/2** — started in EX, with `Rd` and `Rdh` committed across
  two consecutive writeback cycles through the single write port. See
  [`penumbra2/regfile.md`](./penumbra2/regfile.md).

The unit itself is unchanged between them: it presents `o_result_lo`
and `o_result_hi` combinationally once `busy` falls, and the consumer
commits them per its own writeback policy. Writing the high half to
`R0` (the 2-operand form) is dropped by the register file with no
special-casing in either core.

## Arithmetic Faults — `VEC_ARITH`

`o_fault` is a **single-cycle pulse on the start cycle**: the
divisor-zero test is combinational, so a faulting divide never
asserts `o_busy` and never iterates. The contract to the CPU is
generation-neutral: a start that pulses `o_fault` commits nothing and
raises `VEC_ARITH`; a start without it proceeds normally — hold while
`busy`, commit the result when `busy` falls. The fault indication
must be a pulse, not a held level: the CPU latches it at start, and a
persisting `o_fault` would re-trigger the trap after the pending
latch clears.

EPC points at the trapping `DIV` instruction. The kernel handler
delivers `SIGFPE` and leaves EPC alone, so a userland `SIGFPE` handler
that wants to retry can.

**Defensive only.** DIV0 indicates a caller bug — compiler emitting
wrong code, or hand-written code dividing by a zero register. The
hardware fault is a safety net, not a normal control signal. The
implementation has no requirement to deliver the fault quickly — it
only needs to stop the iteration immediately and signal before the next
instruction commits.

When an arithmetic fault races a memory fault in the same instruction,
the priority ordering is:
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
- **RTL (unit):** implemented (`hw/rtl/common/divmul.sv`) — the
  algorithm, handshake, and datapath described above. Promoted to
  `hw/rtl/common/` once Penumbra/2 instantiates it.
- **Penumbra/1 driving:** microcode implemented. Dispatch slots 0x40
  (MUL), 0x42 (MULU), 0x44 (DIV), 0x46 (DIVU) hold the 3-µop routines
  with a shared high-half writeback tail at 0x49; the sequencer drives
  `divmul_start`, stalls on
  `i_divmul_busy`, aborts on `o_fault → VEC_ARITH`, and routes results
  through the `wb_src` mux (`DML_LO`/`DML_HI`).
- **Penumbra/2 driving:** EX hosts the unit behind the same
  start/busy handshake, decoding the 2-bit ISA sub-opcode selector
  into the 5-bit op codes; the two result halves sequence through the
  single regfile write port at WB.
- **ISS:** implements MUL/MULU/DIV/DIVU end-to-end with both halves and
  `VEC_ARITH` (`sw/sim/penumbra_iss.cpp`).
- **LLVM backend:** lowers s32 multiply/divide to the unit — `G_MUL`
  selects `MUL`; `G_SDIV`/`G_SREM` select directly; `G_UDIV`/`G_UREM`
  strength-reduce power-of-two constants and select `DIVU` otherwise;
  `G_SDIVREM`/`G_UDIVREM` select the paired-destination form. 64-bit
  operations remain compiler-rt libcalls.

## See Also

- [`doc/system/instruction-set.md`](../system/instruction-set.md) — programmer-facing
  MUL/DIV reference, assembler syntax, register-pair convention, DIV0 trap
- [`doc/system/instruction-encoding.md`](../system/instruction-encoding.md) — Format R
  opcode partition, sub-encoding with `Rdh`
- [`doc/internals/penumbra1/datapath.md`](./penumbra1/datapath.md) — ALU vs peer-unit
  partitioning, Penumbra/1 micro-sequencer STALL behaviour
- [`doc/system/architecture.md`](../system/architecture.md) — `VEC_ARITH`
  in the exception vector table

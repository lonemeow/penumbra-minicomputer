# Penumbra CPU — MUL/DIV Unit (divmul)

> **Applies to:** all generations · shared peer unit. The algorithm,
> handshake, and internal datapath are identical on every core; *how a
> core drives the unit* — microcode on Penumbra/1, an EX-stage start
> plus a sequenced two-write commit on Penumbra/2 and Penumbra/3 —
> lives in that core's docs.

## Overview

The divmul unit implements hardware multiply and divide for the
Penumbra ISA:

| Instruction | Operation                                                                | Cycles |
|-------------|--------------------------------------------------------------------------|:------:|
| MUL, MULU   | `Rdh:Rd = Rd × Rs` (signed/unsigned 32×32→64; `Rdh=R0` → low half only)  | 2 + 32/`MUL_BITS_PER_STEP` |
| DIV, DIVU   | `Rd, Rdh = Rd / Rs, Rd % Rs` (32/32, signed/unsigned; `Rdh=R0` → quotient only) | 34 |

The multiply retires `MUL_BITS_PER_STEP` multiplier bits per step, a
unit parameter each core picks: one bit per step is what discrete logic
affords, and an FPGA core buys fewer steps with a wider partial-product
reduction. The divide retires one quotient bit per step regardless.

The third operand `Rdh` is always **write-only** — for `MUL`/`MULU` it
receives the high half of the product, and for `DIV`/`DIVU` it receives
the remainder. Writes to `R0` are silently dropped, which is how the
2-operand forms discard the high half for free. Divides are always
32/32; there is no 64/32 narrowing form (see the rationale in
[instruction-set.md](../system/instruction-set.md)).

The unit is a **peer to the ALU**, not part of it: a separate block on
the register-bus fabric with its own operand latches and FSM, driven
by a `start` pulse and reporting completion through a **registered**
`o_done` level (the ALU is single-cycle and always looks done). That
completion joins the unified busy/STALL gate (`cache_busy | … `) the
way any other multi-cycle operation does. Cycle semantics are in
[Completion Signalling](#completion-signalling); how each generation
expresses the driving differs — see
[Driving the Unit](#driving-the-unit-per-generation).

## External Interface

| Signal              | Direction      | Purpose                                                  |
|---------------------|----------------|----------------------------------------------------------|
| `i_a[31:0]`         | CPU → divmul   | First operand: multiplier / dividend                      |
| `i_b[31:0]`         | CPU → divmul   | Second operand: multiplicand / divisor                    |
| `i_op[4:0]`         | CPU → divmul   | Operation select, in the core-internal ALU-field encoding: MUL `01101`, MULU `01110`, DIV `01111`, DIVU `10000` |
| `i_start`           | CPU → divmul   | 1-cycle pulse — latch operands, begin iteration           |
| `o_done`            | divmul → CPU   | Registered. Low while an operation is in flight, high from the cycle it completes until the next start |
| `o_fault`           | divmul → CPU   | Registered. Valid with `o_done`: the completed operation was a divide by zero; raises `VEC_ARITH` |
| `o_result_lo[31:0]` | divmul → W-bus | Low half  (quotient / product low)                        |
| `o_result_hi[31:0]` | divmul → W-bus | High half (remainder / product high)                      |
| `o_flag_n`, `o_flag_z` | divmul → SR | Flag outputs (latched on `flag_w_en` in writeback)        |

The op encoding is the gen1 microcode ALU field, carried directly by
the micro-word; the pipelined cores decode its 2-bit ISA sub-opcode
selector into the same 5-bit codes before driving the unit.

Operand latching is single-cycle: the CPU drives `i_a` and `i_b`
simultaneously, pulses `i_start`, and the unit captures both. Two
operand reads are all the regfile (2R/1W) can supply in one cycle; the
third operand `Rdh` names only the *destination* for the high half and
is consumed by the writeback µ-op, not as a hardware input. Results
reach the register file through the W-bus mux (`wb_src`), alongside the
ALU and load-data sources.

## Completion Signalling

The unit presents a **result bundle** — `o_done`, `o_fault`,
`o_result_lo`, `o_result_hi`, `o_flag_z`, `o_flag_n` — describing the
most recent operation. Every member takes its value on the cycle that
operation completes and holds it until the next accepted start. None
is a pulse, so none has to be caught on a particular cycle: a consumer
back-pressured by something downstream can commit late.

`o_done` is the member that says the rest are meaningful. It and
`o_fault` are **registered**, so on the cycle a start is issued the
bundle still describes the *previous* operation. The consumer holds
that cycle from its own launch decision — it is the party asserting
`i_start` — and consults `o_done` only from the next cycle on.

The unit spends one cycle latching operands, one cycle per iteration
step, and completes on the cycle after the final step. Writing that
cycle as `N`:

| Cycle | `i_start` | `o_done` | `o_fault` | Result outputs | Consumer |
|:-----:|:---------:|:--------:|:---------:|----------------|----------|
| `0`         | 1 | *prev* | *prev* | *prev* | Holds — from its own launch decision, not from `o_done` |
| `1`…`N-1`   | 0 | 0 | 0 | stale | Holds while `!o_done` |
| `N` onward  | 0 | **1** | 0 | **valid** | Commits both halves and the flags; advances |

A divide by zero is not a separate path — it is an operation with
nothing to compute, so it completes on the first cycle it can, with
`o_fault` set instead of results:

| Cycle | `i_start` | `o_done` | `o_fault` | Result outputs | Consumer |
|:-----:|:---------:|:--------:|:---------:|----------------|----------|
| `0`         | 1 | *prev* | *prev* | *prev* | Holds — from its own launch decision |
| `1` onward  | 0 | **1** | **1** | unchanged | Commits nothing; raises `VEC_ARITH` |

One consumer rule covers both: *hold the launch cycle
unconditionally, then hold while `!o_done`; on the first cycle
`o_done` is high, commit the results or take the fault according to
`o_fault`.* A consumer never needs to know the iteration count, and
the unit never needs to know how the consumer stalls.

Because the bundle is held rather than pulsed, a consumer must qualify
`o_fault` with which instruction it is completing — the same
qualification it applies to the result halves before committing them.
An unqualified test raises `VEC_ARITH` on the launch cycle from the
previous divide's flag, and again once the pending flag clears inside
the handler.

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
      │ iter_cnt │   6-bit down-counter, init = the operation's step
      └──────────┘   count; its terminal step registers o_done

      ┌──────────────────────────────────────────┐
      │  Fault detect (evaluated at start,       │
      │  registered into the result bundle):      │
      │    fault ← (op ∈ DIV/DIVU) && (B == 0)   │
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
| MUL   | \|A\| (multiplier mag) | 0          | \|B\| (multiplicand mag) | 32/`K` |
| MULU  | A                      | 0          | B                        | 32/`K` |
| DIV   | \|A\| (dividend mag)   | 0          | \|B\| (divisor mag)      | 32     |
| DIVU  | A (dividend)           | 0          | B (divisor)              | 32     |

`accum_hi` always starts at 0. Signed `MUL` and signed `DIV` iterate on
the *magnitudes* of their operands and apply the sign on the output
side. The iteration loop is identical across all four operations once
the registers are initialized.

## Multiplication — shift-add (unsigned)

Writing `K` for `MUL_BITS_PER_STEP`:

```
for iter in 32/K:
    accum_hi ← accum_hi + Σ(j = 0..K-1) accum_lo[j] ? (divisor << j) : 0
    {accum_hi, accum_lo} ← {accum_hi, accum_lo} >> K   ; ("divisor" holds multiplicand)
```

After `32/K` iterations `accum_hi:accum_lo` holds the full 64-bit
product. Widths are exact: the high half plus every row is bounded by
`(2^32 - 1) * 2^K`, so the shifted concatenation loses nothing.

`K = 1` is the single conditional add of the classic shift-add walk.
Above that the rows want reducing in **carry-save**, resolving through
one adder at the end of the step — chaining a carry-propagate add per
row instead costs a full adder delay per bit retired, which is most of
what retiring more bits per step was meant to buy.

For **signed** MUL: sign-magnitude — negate both operands at the start
if negative, multiply, negate the product if signs differed. (Booth's
algorithm eliminates the negate cycles but adds gates.)

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

How a core sequences `start`, the launch-cycle hold, the wait for
`o_done` and the two-result commit is generation-specific and lives in
that core's docs. What every generation shares is the rule in
[Completion Signalling](#completion-signalling):

- **Penumbra/1** — a 3-µop microcode routine per opcode (dispatch slots
  `0x40`–`0x46` with a shared high-half writeback tail). See the
  *MUL/DIV dispatch routines* in
  [`penumbra1/microcode.md`](./penumbra1/microcode.md#muldiv-dispatch-routines).
- **Penumbra/2** — started in EX, with `Rd` and `Rdh` committed across
  two consecutive writeback cycles through the single write port. See
  [`penumbra2/regfile.md`](./penumbra2/regfile.md).
- **Penumbra/3** — started in EX, which holds until the unit is done,
  with `Rd` and `Rdh` committed across two writeback cycles. See the
  [pipeline shape](./penumbra3/overview.md#pipeline-shape), and for
  what the second write costs,
  [`penumbra3/not-built.md`](./penumbra3/not-built.md).

The unit itself is unchanged between them: it presents `o_result_lo`
and `o_result_hi` from the cycle `o_done` rises until the next accepted
start, and the consumer commits them per its own writeback policy. Writing the high half to
`R0` (the 2-operand form) is dropped by the register file with no
special-casing in either core.

## Arithmetic Faults — `VEC_ARITH`

`o_fault` is a **held member of the result bundle**, valid with
`o_done` and cleared only by the next accepted start. The
divisor-zero test is evaluated combinationally at start but reaches
the output through a flop. A faulting divide is not a separate path:
it is an operation with nothing to compute, so it iterates zero times
and completes on the first cycle it can, which is where the consumer
is already looking for results or a fault. The contract to the CPU is
generation-neutral and identical to the non-faulting one: hold the
launch cycle, then hold while `!o_done`; on the first cycle `o_done`
is high, `o_fault` says whether to commit the results or raise
`VEC_ARITH`. Because it is held rather than pulsed, the consumer must
qualify it with which instruction it is completing — otherwise it
would re-raise the trap once the pending flag clears in the handler.

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

At one multiplier bit per step. Each extra bit costs a gated row and a
carry-save level — a row of AND gates plus a rank of full adders per
32-bit slice — which is why the discrete build stays at one and the
count below does not track the FPGA cores.

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
  algorithm, handshake, and datapath described above, with the
  multiply step count set per instantiation by `MUL_BITS_PER_STEP`. Promoted to
  `hw/rtl/common/` once Penumbra/2 instantiates it.
- **Penumbra/1 driving:** microcode implemented. Dispatch slots 0x40
  (MUL), 0x42 (MULU), 0x44 (DIV), 0x46 (DIVU) hold the 3-µop routines
  with a shared high-half writeback tail at 0x49; the sequencer drives
  `divmul_start`, stalls on
  `i_divmul_done`, aborts on `o_fault → VEC_ARITH`, and routes results
  through the `wb_src` mux (`DML_LO`/`DML_HI`).
- **Penumbra/2 driving:** EX hosts the unit behind the same
  start/done handshake, decoding the 2-bit ISA sub-opcode selector
  into the 5-bit op codes; the two result halves sequence through the
  single regfile write port at WB.
- **Penumbra/3 driving:** the same, from its own EX stage and WB.
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

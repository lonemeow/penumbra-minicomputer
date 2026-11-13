# Penumbra/4 — The load-use wait and a second ALU: measurements

> **Applies to:** Penumbra/4 · execute and memory stages (ideas and
> measurements; nothing is built). The wait measured is gen3's.

A load's data comes out of block RAM too late to forward into the ALU
in the same cycle, so in gen3 an op that reads a register directly
behind the load that writes it waits two cycles
([gen3 memory completion](../penumbra3/memory-completion.md#load-hit--a-registered-load-use-hazard)).
This records what that wait costs on real workloads and what a second
ALU, placed where the load's data arrives, would take back.

The second ALU is the design of Western Digital's SweRV EH1, a two-wide
in-order RISC-V core with the same constraint: RAMs with a two-cycle
access and no forward from the RAM's output into the first ALU
([write-up](https://tomverbeure.github.io/2019/03/13/SweRV.html)). An
ALU op whose operand a load has not delivered does not wait at EX. It
moves on and executes at a second ALU, in the stage where the data
arrives. A branch that resolves there pays the longer redirect.

## Measurement setup

Measured 2026-10-04 with the ISS's load-use model (`+loadusemodel`) on
the two windows [issue-pairing](./issue-pairing.md) uses:
`pbench kernel` (719 M instructions) and Doom `demo3` (2.89 G).

The model is a single-issue core. Each instruction takes the EX slot
after the one before it unless it has to wait for a register:

- **An ALU result** is usable by the next instruction.
- **A load's value** is usable `wait` cycles later than that: 2 in
  gen3, 1 for comparison (`wait:1`).
- **A load's or store's base and a jump's target** are needed at EX,
  and store data at the late stage, `1 + wait` stages after EX, where
  it never waits.
- **With one ALU, at EX** (`ex`), an op waits there for its operands.
- **With a second ALU** at the late stage (`ex+late`), an ALU op whose
  operand is not there at EX executes late instead of waiting, and its
  result is usable that much later. A conditional branch whose flags
  are late resolves late, and a misprediction then costs `1 + wait`
  cycles more. Mispredictions come from a pre-decode redirect with a 1K
  bimodal table, gen3's planned front end.
- **With the one ALU after the memory stages** (`late`), every ALU op
  executes late: the order of the 486 and its successors, where an
  address is formed a stage ahead of the cache and the ALU sits behind
  it.

What it leaves out: every access hits, fetch always supplies the next
instruction, and `MUL` and `DIV` read their operands at EX and count as
one cycle. An op goes late whenever its operand is missing, with no
look at what reads its result.

## What the wait costs

Cycles waited per 1000 instructions, with the late mispredictions'
extra cycles included, as `pbench kernel` / Doom:

| | 2-cycle wait | 1-cycle wait |
|---|---|---|
| One ALU, at EX | 131 / 147 | 59 / 71 |
| A second ALU behind the memory stages | 31 / 134 | 15 / 83 |
| One ALU, behind the memory stages | 158 / 380 | 97 / 247 |

Who waits, at the 2-cycle wait:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| One ALU: ALU ops waiting for an operand | 113 | 143 |
| One ALU: loads and stores waiting for a base | 14 | 3 |
| Second ALU: ALU ops executed late, of all ALU ops | 78 of 462 | 83 of 612 |
| Second ALU: loads and stores waiting for a base | 17 | 123 |
| Second ALU: conditional branches resolved late, of all | 49 of 109 | 25 of 91 |
| Second ALU: resolved late and mispredicted | 2.1 | 1.6 |

- **gen3's wait costs 131–147 cycles per 1000 instructions.** On the
  kernel paths that is what an L1 size step is worth in gen3's
  [lever table](../penumbra3/overview.md#levers-ranked-by-measured-benefit),
  and on Doom about four times one.
- **A second ALU takes back about 100 on the kernel paths and 13 on
  Doom.** Kernel code loads a field and tests or adds to it, and the
  late ALU absorbs that. Doom's loops load, add, and load through the
  sum: the second load needs its base at EX, and an add executed late
  delivers it later than one that had waited.
- **Late branches cost little.** Nearly half the kernel paths'
  conditional branches resolve late, but about 2 per 1000 instructions
  are also mispredicted: 5–6 cycles.
- **Shortening the wait to one cycle is worth 71–76 on both
  workloads,** and at that wait a second ALU costs Doom more than it
  saves.

## Ruled out

- **One ALU behind the memory stages.** 158–380 at the 2-cycle wait.
  The ISA has no indexed addressing, so address arithmetic is ALU ops,
  and an ALU behind the cache makes every computed address wait for it.
- **A second ALU behind the memory stages, for gen4.** It takes back
  about 100 of the kernel paths' 131 and 13 of Doom's 147, but it needs
  a stage of its own behind the memory stages, and a conditional branch
  whose flags it computes resolves there, three stages after EX. With
  its memory stalls cut, gen3 loses a large share of its cycles to
  branches, and a later place to resolve them adds to that. The model
  above charges the late branches 5–6 cycles, but it sees every access
  hit and nothing stall. It is a candidate for a later generation,
  weighed on gen4's own counters.

## What it means for pairing

SweRV issues a load and the ALU op that reads its result together: the
reader executes at the late ALU. It does the same behind a multiply,
and pairs a store with the ALU op that produces its data, since store
data is needed late. It does not pair an ALU op with an ALU op it
depends on; its decode logic (`dec_decode_ctl.sv` in the released RTL)
carries that case commented out as bad for performance.

The pairing model counts issue slots and never pairs an op with a load
it reads ([what still issues alone](./issue-pairing.md#what-still-issues-alone)).
With a second ALU those pairs become possible, so the two measurements
do not add: what a late ALU saves in waits and what it adds in pairs
need one model that holds both.

## Design

- **One ALU in each pipeline, at EX, behind gen3's two-cycle wait.**
  The second ALU is [ruled out](#ruled-out) for gen4.
- **The core counts its load-use waits** by what waited: an ALU
  operand, a base, a branch's flags or a jump's target, the needs this
  model reports. The model sees no miss and no stall, and the counts
  from gen4 itself are what a later generation weighs a second ALU on.

## Open

- **One model for pairing and waits.** A pair whose younger op waits on
  a load holds both ops.
- **The compiler's share.** A schedule that moves an independent
  instruction between a load and its reader hides the same cycles, and
  the machine scheduler is not enabled
  ([per-generation tuning](../../TODO/compiler-pipeline.md#the-largest-divergence-dependence-distance)).
  How often such an instruction is there to move is not measured.
- **A one-cycle wait.** Worth 71–76 with one ALU, but it means the
  load's data reaching EX's operand mux a cycle sooner, the path gen3
  keeps registered.

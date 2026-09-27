# Scheduling models and the timing reference

The backend describes each generation of the core with a scheduling
model in `llvm/llvm/lib/Target/Penumbra/Penumbra.td`. The figures a
model states come from the programmer's timing reference,
[`system/instruction-timing.md`](../system/instruction-timing.md), and
a change to a figure there is carried into the model in the same
change (the rule is recorded in `hw/CLAUDE.md`, "Instruction cycle
counts feed the compiler").

## One model per generation

`penumbra1`, `penumbra2` and `penumbra2_5` are processors with an empty
architecture list: one ISA, so they differ only in their tuning. A
`PenumbraTuning` record pairs a model with its tune features, so a
name that borrows another generation's tuning — `generic` borrows
`penumbra2_5`'s — borrows both halves together. `-mcpu` and `-mtune`
accept every name.

Every model shares `PenumbraSchedModel`'s parameters: single issue,
in order (`MicroOpBufferSize = 0`), `LoadLatency = 1`,
`HighLatency = 8`, `MispredictPenalty = 0`, and an incomplete model,
so an instruction without a scheduling class takes the defaults.

## What a model states

Three write classes carry a latency. The reference gives each
instruction an issue figure and a result latency; the models state the
issue figure here:

| Class | Instructions |
|---|---|
| `WriteALU` | ALU operations, register and immediate forms |
| `WriteMul` | `MUL`, `MULU` |
| `WriteDiv` | `DIV`, `DIVU` |

One read class, `ReadALU`, carries a `ReadAdvance`: the part of an
ALU result's latency that reaches past its producer's issue — nothing
on `penumbra1` and `penumbra2_5`, three cycles on `penumbra2`.

Loads, stores and control transfers carry no scheduling class, so
their costs and the waits on a load's result are not represented.

## Who reads it

`PenumbraSubtarget` reads the latencies back as `getALUCycles`,
`getMulCycles` and `getDivCycles`, and prices a wide constant as
`getWideConstantCycles` (`LLI`, the `ReadALU` wait for its result,
then `LUI`). The consumers are cost decisions, not instruction order:

- `PenumbraTTIImpl` weighs multiplies and divides against ALU
  operations for the IR-level cost model;
- `isIntDivCheap()` compares a divide with the multiply-based rewrite
  that would replace it;
- the constant-multiply combine
  ([strength-reduction.md](strength-reduction.md)) decomposes a
  multiply into ALU operations when that is cheaper on the tuned core;
- the wide-constant price decides whether a constant is rebuilt where
  it is used or kept live in a register.

No instruction scheduling pass runs: the subtarget does not enable the
machine scheduler, and GlobalISel has no scheduler of its own, so
instructions keep their selection order.

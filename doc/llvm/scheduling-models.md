# Scheduling models and the timing reference

The backend describes each generation of the core with a scheduling
model in `llvm/llvm/lib/Target/Penumbra/Penumbra.td`. The figures a
model states come from the programmer's timing reference,
[`system/instruction-timing.md`](../system/instruction-timing.md), and
a change to a figure there is carried into the model in the same
change (the rule is recorded in `hw/CLAUDE.md`, "Instruction cycle
counts feed the compiler").

## One model per generation

`penumbra1`, `penumbra2`, `penumbra2_5` and `penumbra3` are processors
with an empty architecture list: one ISA, so they differ only in their
tuning. A `PenumbraTuning` record pairs a model with its tune
features, so a name that borrows another generation's tuning —
`generic` borrows `penumbra2_5`'s — borrows both halves together.
`-mcpu` and `-mtune` accept every name.

Every model shares `PenumbraSchedModel`'s parameters: single issue,
in order (`MicroOpBufferSize = 0`), `LoadLatency = 1`,
`MispredictPenalty = 0`, and an incomplete model, so an instruction
without a scheduling class takes the defaults.

## What a model states

A model states the figures the reference gives an instruction: its
issue, as the cycles the instruction holds the model's one resource,
the core, and for one that writes a register its latency, as the
latency of its write. The core is a single unit that nothing queues
for (`BufferSize = 0`), so an instruction holding it holds every
instruction behind it.

The write classes carry the figures:

| Class | Instructions | Figures |
|---|---|---|
| `WriteALU` | ALU operations, register and immediate forms | issue, latency |
| `WriteLoad` | `LDW`, `LDH`, `LDHS`, `LDB`, `LDBS` | issue, latency |
| `WriteStore` | `STW`, `STH`, `STB` | issue |
| `WriteMul` | `MUL`, `MULU` | issue, latency |
| `WriteMulHigh` | the high half a pair form writes | latency |
| `WriteDiv` | `DIV`, `DIVU` | issue, latency |
| `WriteDivHigh` | the remainder a pair form writes | latency |

A pair form's second write states no issue: the instruction's issue
is its first write's, and the cycles of two writes on one resource
add up.

One read class, `ReadALU`, carries a `ReadAdvance` for a reader that
gets a result sooner or later than its producer's latency says. Every
model states 0.

Control transfers and the system instructions carry no scheduling
class.

## Who reads it

`PenumbraSubtarget` reads the issue figures back as `getALUCycles`,
`getMulCycles` and `getDivCycles`, and prices a wide constant as
`getWideConstantCycles`: `LLI`, the part of its latency that `LLI`'s
issue does not cover, then `LUI`. The consumers are cost decisions,
not instruction order:

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

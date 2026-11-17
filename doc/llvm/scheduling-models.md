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
without a scheduling class takes the defaults. The pipelined
generations' models, `penumbra2`'s, `penumbra2_5`'s and
`penumbra3`'s, derive from `PenumbraPipelinedModel`, which adds
`PostRAScheduler = 1`.

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
issue does not cover, then `LUI`. Those consumers are cost decisions:

- `PenumbraTTIImpl` weighs multiplies and divides against ALU
  operations for the IR-level cost model;
- `isIntDivCheap()` compares a divide with the multiply-based rewrite
  that would replace it;
- the constant-multiply combine
  ([strength-reduction.md](strength-reduction.md)) decomposes a
  multiply into ALU operations when that is cheaper on the tuned core;
- the wide-constant price decides whether a constant is rebuilt where
  it is used or kept live in a register.

The other reader is the machine scheduler after register
allocation, which orders instructions by both figures. It runs for a
tuned core whose model sets `PostRAScheduler`: `penumbra2`,
`penumbra2_5` and `penumbra3`, and so `generic`. GlobalISel has no
scheduler of its own and the machine scheduler before allocation
stays off, so on `penumbra1` instructions keep their selection order.
`-mllvm -enable-post-misched=false` turns the scheduler off for a
build to compare against, and `=true` on for any model.

## What the scheduler moves

The scheduler orders each region, a stretch of a block between calls
and other boundaries, top down, so that a reader starts as late after
its producer as the producer's latency asks.

Measured 2026-10-04 on the ISS, scheduling for `penumbra2_5`: no
workload's instruction count moves by more than 0.13%, and 54% of
Doom's load-use pairs go, 53% of Dhrystone's, 27% of CoreMark's and
18% of the kernel's. A pair is a load whose value the next
instruction reads. Most pairs move to distance 2, which is enough on
`penumbra2_5`; `penumbra3` waits a cycle there.

Counted 2026-11-16 over Doom's compiled code, each basic block once
with the reference's figures. Scheduled for `penumbra3`, its
instructions wait 38% fewer cycles on `penumbra3` than unscheduled,
and 10% fewer than scheduled for `penumbra2_5`. On `penumbra2_5`, the
`penumbra3` schedule waits 4% more than `penumbra2_5`'s own. Bottom
up, the code waits 10% more on `penumbra3` than top down, and in both
directions 25% more.

The dependency graph uses alias analysis (`useAA()`), so a load can
move above a store it provably does not overlap. Counted the same
way, that takes 4.7% of the waits the schedule leaves in Doom's code
on `penumbra2_5` and 3.8% of the kernel's, and lengthens no
function's. The kernel builds with `-fno-strict-aliasing`, so there
it separates only fields behind one pointer and distinct objects.
The analysis trusts each access's memory operand: one the backend
builds describes the access exactly or names no value.

A loop gives the scheduler only its own body. The column and span
drawers keep three of their 15 and 18 cycles a pixel waiting under
either schedule: a pixel is one chain of dependent loads and the
store they feed, the store comes before the increment of its base,
and the counter's flags come last before the branch.

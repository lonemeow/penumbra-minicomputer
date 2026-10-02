# Penumbra/4 — Fusion and pairing: measurements and design

> **Applies to:** Penumbra/4 · issue stage (ideas and measurements;
> nothing is built).

A dual-issue core issues two ops in a cycle when the second may join
the first, and its front end can fuse two adjacent instructions into
one op. This records what each fusion and each pairing rule is worth,
measured on real workloads with the ISS's pairing model, and the issue
stage the numbers point to. The machine around it is in the
[overview](./overview.md).

## Measurement setup

Measured 2026-10-01 with the ISS's `+pairmodel` (`sw/sim/pair_model.h`),
which runs every executed instruction through shadow two-slot issue
stages side by side and counts the issue cycles each would take. The
workloads, on the multiuser image with `+core=penumbra2_5`, driven by
`sw/tools/iss-workloads.py`:

- **`pbench kernel`:** one window around the command, about 725 M
  instructions.
- **Doom `demo3`:** `doom -iwad DOOM1.WAD -timedemo demo3` from the
  first `R_RenderPlayerView` to `G_CheckDemoStatus` (`+model_start`,
  `+model_stop`), 2.93 G instructions.

Every figure is **issue cycles per 1000 instructions**, given as
`pbench kernel` / Doom. A single-issue core without fusion takes 1000.

What the figures leave out:

- **They are issue slots only.** No stall, miss or redirect is
  modelled, and fetch always has the next instruction ready. A taken
  transfer ends a pair; that is the front end's only effect.
- **Pairing is greedy:** an op joins the op ahead of it whenever it
  may.
- **The streams carry gen2.5's TLB refills.** The ISS runs the 32×2
  TLB, and at the refill rates in gen3's
  [memory-performance](../penumbra3/memory-performance.md#fills-and-refills-at-gen3s-geometries)
  the refill handler is an estimated 15 % of Doom's instructions and
  25 % of `pbench kernel`'s. A third of the handler is system
  instructions, which issue alone, so a stream behind gen3's larger TLB
  pairs somewhat better than these.

A stage is written as the model's item: `dual` has slot X issue simple
ALU ops, shifts and moves, and slot Y everything, with the older op in
X; `fw:` adds same-cycle forwards from the older op to the younger,
`fuse:` fusions, `flex` lets either op take either slot, and
`x:simple:ld` with `mem:ld2` gives X a load port.

## What the streams hold

Instructions per 1000, by the class a slot has to issue:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| ALU ops and compares | 229 | 362 |
| Shifts | 86 | 91 |
| Moves and immediates | 167 | 156 |
| Loads | 152 | 159 |
| Stores | 91 | 76 |
| Branches and jumps | 135 | 105 |
| System instructions | 134 | 46 |

`ADC`/`SBC` and `MUL`/`DIV` are under 10 between them. Doom's system
instructions are nearly all the refill handler's.

## Fusion on one issue slot

Three fusions, with one issue slot behind a front end that supplies two
instructions a cycle:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| No fusion | 1000 | 1000 |
| `LLI Rd; LUI Rd` as one constant | 963 | 968 |
| … plus a compare and its `Bcc`, and `MOV Rd, Rs` with the op that updates `Rd` in place | 839 | 866 |

Each fusion's own share: the constant 37 / 32, compare-and-branch
91 / 42, `MOV` and op 34 / 60.

- **Fusion is worth 134–161 cycles per 1000 with no second execution
  pipeline.** That is more than plain dual issue gains without it
  (98–121, in the next table).
- **It still needs the wider front end.** A fused op saves its cycle
  only when both of its instructions were fetched in one, so fusion is
  no lever for a core that reads one instruction a cycle.
- **It takes back what the instruction format costs.** A 32-bit
  constant is one op again, and a copy followed by a two-operand op is
  the three-operand op the encoding has no room for.
- **The dependency inside an `LLI`/`LUI` pair is one of naming.** `LLI`
  writes the low half and clears the high one; `LUI` ORs the high half
  in. No bit of the result depends on another, so the pair is one
  constant known at decode.

## Pairing through forwards

Dual issue without fusion, adding same-cycle forwards from the older op
to the younger:

| Stage | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `dual+yflags`: only the younger op commits flags | 944 | 923 |
| `dual`: the youngest flag writer's flags stand | 902 | 879 |
| … `+fw:cond`: the flags, to a branch's condition | 829 | 813 |
| … `+fw:cond:mov:imm:data`: also a copy's source, an immediate, store data | 760 | 739 |
| … `+fuse:lilui` | 739 | 711 |
| … `+flex` | 719 | 696 |
| … `+x:simple:ld+mem:ld2` | 691 | 685 |

- **The compare and its branch are the largest step,** 66–73. Copies
  and immediates together are worth as much again: the pairs relying
  on the copy forward number 32 / 50 per 1000 instructions, on the
  immediate forward 39 / 27, on store data 5 / 1.
- **Each of these forwards is a path from one pipeline into the other
  inside a cycle.** [Fusion](#fusion-and-dual-issue-together) reaches
  the same idioms without them.

## Fusion and dual issue together

| Stage | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `single+fuse:lilui:cmpbr:movop` | 839 | 866 |
| `dual+fuse:lilui:cmpbr:movop`, no forwards | 744 | 746 |
| … `+flex` | 700 | 676 |
| … `+fw:imm` | 681 | 668 |
| … `+x:simple:ld+mem:ld2` | 638 | 652 |
| The last two with `fw:cond:mov:imm:data` in place of `fw:imm` | 672, 629 | 663, 647 |

- **Fused ops need almost no forwards.** With the three fusions and
  `flex`, the condition, copy and store-data forwards together gain
  5–9. The immediate forward gains 8–19, and its value is known at
  decode.
- **`flex` earns its place once ops are fused:** 44–70, against 15–20
  on the forwards ladder. A fused compare-and-branch is an op only Y
  issues, and `flex` lets a simple op behind it take X.
- **A second load port is worth 16–43,** most on the kernel paths.
- **With one memory access a pair, the stream issues in 668–681 cycles
  per 1000 instructions,** 1.47–1.50 instructions a cycle.

### The ceiling

With every class in both slots, any two memory accesses a cycle, every
forward including an older ALU result as the younger op's ALU operand,
and the fusions: 555 / 575. Of what separates it from the stages above,
chained ALU ops are the largest part, 51 / 74 pairs per 1000
instructions: two ALUs in series in one cycle.

### What still issues alone

Ops that issue alone, per 1000 instructions, in
`dual+x:simple:ld+fuse:lilui:cmpbr:movop+fw:imm+flex+mem:ld2`, by what
kept the op behind from joining:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| It or the op behind it is a system instruction | 164 | 56 |
| The op behind it needs its result as an ALU operand | 99 | 165 |
| … or as a load's or store's base | 22 | 86 |
| It is a taken transfer | 35 | 62 |
| The two would make two memory accesses, not both loads | 26 | 10 |
| A trap or interrupt comes between them | 11 | 4 |
| More than one cause | 62 | 47 |

The two dependency rows are sequences that are serial in any case: a
shift feeding an add feeding a load, as in Doom's column and span
loops.

## A flag-setting op and its branch

Measured 2026-10-01 with a scratch patch to the ISS, not in the tree: a
count of every `Bcc` whose flags come from the instruction directly
ahead, and a fourth fusion taking an ALU op that also writes a register
with the `Bcc` behind it.

Per 1000 instructions:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| A compare directly ahead of its `Bcc` | 90.8 | 42.2 |
| … nothing flag-neutral could be moved between them | 79.8 | 33.9 |
| Another op directly ahead of its `Bcc` | 4.0 | 36.0 |
| … an `ADD` or `SUB #imm` | 1.7 | 35.9 |
| … the branch taken | 1.5 | 34.8 |
| … a `MOV` or immediate could be moved between them | 0.1 | 0.0 |
| … only a load or store could | 1.4 | 35.5 |
| Issue cycles the fourth fusion removes | 3–4 | 36 |

- **On Doom these are loop ends:** a counter update and its `BNE`, once
  a pixel in the column and span loops.
- **Reordering does not stand in for fusing them.** A store could move
  between the counter update and the branch in nearly every case, but
  a store is no partner for a branch: X does not issue it. The loop
  tail keeps its op count, and what the reordering gains, `flex`
  already gains in hardware. Fusion removes an op, and its 36 cycles
  come off every dual stage measured, with `flex` or without.
- **It asks nothing new of the datapath.** A compare is a subtraction
  with its register write suppressed, so the fused compare-and-branch
  already holds the adder, the flags, the condition and the branch;
  this fusion leaves the write enabled. No shift fed a branch on Doom.
- **Compares cannot be scheduled away from their branches either:**
  for 80–88 % of them nothing could go between.

The count of what could be moved looks back four instructions, never
moves a load past a store, and cannot see a branch target between the
two positions, so it is an upper bound.

## Ruled out

- **Only the younger op commits flags** (`yflags`). It keeps a pair
  apart whenever the older op alone writes the flags, and gives up
  about 40 % of plain dual issue's gain (944 / 923 against 902 / 879).
  What it would save is a four-bit mux with a select known at decode;
  the rule itself is at least that much logic in the pairing decision.
- **A three-input address add** (`fw:ea`), carrying an `ADD` or
  `SUB #imm` result to a base in the same cycle: 3–7.
- **Same-cycle forwards of flags, copies and store data** between the
  two pipelines: 5–9 once the three fusions are in.
- **Compiler scheduling in place of the op-and-branch fusion.** Every
  ALU op writes the flags, so the flag setter is the last ALU op ahead
  of its branch and only loads, stores and moves can sit between them
  ([above](#a-flag-setting-op-and-its-branch)).

## Design

- **Fuse first.** The front end fuses `LLI`/`LUI`, an ALU op and the
  `Bcc` behind it, compares included, and a `MOV` with the op that
  updates its copy.
- **Two slots, either order.** X issues simple ALU ops, shifts and
  moves; Y everything.
- **One forward between the pipelines:** a constant the older op loads,
  wherever the younger reads it.
- **The youngest flag writer's flags stand.**
- **One memory access a pair to start with.** A second load port is the
  next step, 16–43.
- **Pre-decode decides.** Fusing and pairing depend only on the
  instructions, so both are settled at the fetch FIFO's enqueue.

## Open

- **The stream behind a 256×2 TLB.** A report split by privilege, or
  one that leaves the refill handler's page out, would show what the
  handler's instructions hide.
- **The op-and-branch fusion in the model.** Its `cmpbr` takes
  flag-only compares; the figures above came from a scratch patch.
- **Stalls.** A pair whose younger op waits on a load holds both ops,
  and the load-use distance decides how often.
- **Greedy pairing.** With `flex`, a store ahead of a compare takes the
  compare and leaves its branch alone. The cycle count is the same
  either way on these loops, but a rule that looks one op further might
  not be.
- **The fused compare-and-branch's path:** adder, flags, condition and
  resolve in one cycle, to be timed.
- **Other workloads.** The shell loop was not run.

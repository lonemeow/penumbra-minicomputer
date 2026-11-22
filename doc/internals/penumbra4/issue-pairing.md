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
  instructions, which issue alone. Behind gen3's larger TLB the
  stream pairs a little better
  ([measured](#behind-gen3s-tlb)).

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

A fourth fusion, `fuse:opbr`, takes an ALU op that also writes its
register with the `Bcc` behind it. Beside its figures is a count, made
2026-10-01 with a scratch patch to the ISS that is not in the tree, of
every `Bcc` whose flags come from the instruction directly ahead.

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
| Issue cycles `fuse:opbr` removes | 3–4 | 36 |

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

## Address and shift fusions

Measured 2026-11-09 on the same windows, from the base image of that
day, behind gen3's TLB (`+core=penumbra3`, 256×2) and behind gen2.5's
(`+core=penumbra2_5`, 32×2) side by side. Behind gen2.5's, the stage
the [front end](./front-end.md) measures,
`dual+fuse:lilui:cmpbr:movop:opbr+fw:imm+flex`, takes 677 / 631, where
it took 679 / 630 on 2026-10-04. Figures in this section are behind
gen3's TLB unless they say otherwise.

Five more fusions, each of an idiom the encoding spends two
instructions on:

- **`ext`:** `SHL Rd, #n` and `SHR` or `SAR Rd, #n`, a zero or sign
  extension of the low 32 − n bits. The compiler emits the `SAR` form
  for every sign extension in a register.
- **`shadd`:** `SHL Rd, #1..3` and `ADD Rd, Rs`, an index scaled into a
  sum.
- **`addmem`:** `ADD Rd, Rs` and a load or store with `Rd` as its base:
  the base-plus-index access the ISA has no encoding for. A load fuses
  when it overwrites `Rd`.
- **`addwb`:** the same with the load into another register, so the
  fused load also writes its address to `Rd`.
- **`incmem`:** `ADD` or `SUB Rd, #imm` and the access through `Rd`, a
  pre-incremented access.

Fused per 1000 instructions, each alone on one slot:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `ext` | 1.1 | 0.7 |
| `shadd` | 8.0 | 2.7 |
| `addmem` | 5.9 | 73.2 |
| `addwb` | 2.4 | 20.8 |
| `incmem` | 12.4 | 3.6 |

Issue cycles per 1000 instructions, added to the stage the front end
measures:

| Stage | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `dual+fuse:lilui:cmpbr:movop:opbr+fw:imm+flex` | 674 | 616 |
| … `+ext` | 674 | 616 |
| … `+shadd` | 671 | 614 |
| … `+addmem` | 668 | 543 |
| … `+ext:shadd:addmem` | 666 | 542 |
| … and `addwb` | 664 | 524 |
| … and `incmem` | 654 | 539 |
| … all five | 653 | 521 |
| … all five, `+x:simple:ld+mem:ld2` | 582 | 505 |

- **`addmem` is worth 72 on Doom,** 12 % of its issue cycles and more
  than any of the first four fusions there. The column and span loops
  form an address in a register and load through it. With `ext`,
  `shadd` and `addmem`, the ops issuing alone only because the op
  behind reads their result fall from 161 to 90 per 1000 instructions
  as an ALU operand, and from 94 to 51 as a base. On the kernel paths
  `addmem` is worth 6.
- **`addwb` adds 18 on Doom and `incmem` 11 on the kernel paths,** and
  each little on the other workload. All five take 21 and 95 from the
  four fusions' stage: 3 % and 15 %.
- **`ext` and `shadd` are worth 0–3.** A sign-extended byte or halfword
  arrives from `LDBS` or `LDHS`. The extensions in a register are
  hundreds of sites in the kernel, `libc` and Doom that seldom run, and
  those that do run sit behind the `MOV` that copies their operand,
  which `movop` takes first.
- **A fused load that writes its address back writes two registers,**
  and pairs only with an op that writes none. That alone keeps 0.2 /
  2.8 ops per 1000 instructions from pairing.

### The address adder

A fused access adds the add's two inputs and its own offset, three
inputs, and gives the `ADD`'s flags from the first two. With two
inputs (`agu:2` in the model), only an access with no offset fuses,
and its one sum is both the address and the flags.

The adder sits in EX. gen3 forms an access's address there and
registers it ahead of the TLB read and the cache index
([memory stage](../penumbra3/mem-stage.md#stage-contract)), so an
address fusion changes what EX adds and nothing the MMU reads. With two
inputs the address is the sum the `ADD` forms anyway.

| Fused per 1000 instructions, alone on one slot | Any offset | No offset |
|---|---|---|
| `addmem` | 5.9 / 73.2 | 5.8 / 72.7 |
| `addwb` | 2.4 / 20.8 | 1.4 / 16.9 |
| `incmem` | 12.4 / 3.6 | 1.8 / 0.5 |

- **`addmem` needs no third input:** 98–99 % of its accesses have no
  offset.
- **`incmem` nearly always has one.** Its shape is a prologue's: of the
  kernel's 6,655 static sites of an `ADD` or `SUB #imm` and an access
  through its register, 6,288 are `SUB SP, #frame` and a save, 5,538
  of them to `[SP + k]` with k other than 0.
- **With every fusion, two inputs cost 11 / 7:** 664 / 528 against 653 /
  521. Without `addwb` and `incmem` they cost nothing (666 / 542 either
  way).

### Fusion and the second slot add up

| Issue cycles saved per 1000 instructions | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `single+fuse:lilui:cmpbr:movop:opbr:ext:shadd:addmem` | 172 | 251 |
| `dual+fw:imm+flex` | 192 | 218 |
| The two added | 365 | 469 |
| `dual+fuse:lilui:cmpbr:movop:opbr:ext:shadd:addmem+fw:imm+flex` | 334 | 458 |

Together the two take 92 % and 98 % of their sum, and 93 % and 97 %
behind gen2.5's TLB. Fusion takes adjacent instructions that depend on
each other and the second slot adjacent ones that do not, so they
seldom reach for the same cycle.

### Behind gen3's TLB

- **The refill handler is 15 % of Doom's stream** behind gen2.5's TLB:
  the window is 2.92 G instructions there and 2.49 G behind gen3's.
  The kernel paths size their own runs, and keep their length.
- **Pairing gains little from it.** The stage the front end measures
  takes 674 / 616 against 677 / 631. Ops issuing alone beside a system
  instruction fall from 132 to 47 per 1000 instructions on the kernel
  paths, and from 55 to 1 on Doom.
- **`addmem`'s fusions fall from 15 to 6 per 1000 instructions on the
  kernel paths:** each refill runs two of its pairs, `ADD` then `LDW`
  through the sum, for the page directory and the page table.
- **A second load port is worth more,** with `ext`, `shadd` and
  `addmem` added to the four fusions: 72 against 52 on the kernel
  paths, and 17 against 15 on Doom.

## Branches last, and two reads an op

Measured 2026-11-11 on the same windows and image as the
[address fusions](#address-and-shift-fusions), behind gen3's TLB, from
the design's stage, `dual+fuse:lilui:cmpbr:movop:opbr:addmem:addwb+fw:imm+flex`,
which takes 666 / 525. Two rules restrict it:

- **A branch is always the younger op of its pair** (`last:br`). A
  taken transfer already ends a pair; the rule also keeps the op behind
  a branch that is not taken out of the branch's cycle. A redirect then
  never splits a pair: a branch that resolves wrong in EX flushes what
  follows its pair, and its partner, the older, commits.
- **No fused op reads three registers** (`reads:2`). No instruction
  reads more than two, and of the fusions only an address fusion's
  store would: an `ADD` of two registers and a store of a third through
  the sum. Refusing that one keeps a pair to the register file's four
  read ports ([register file](./regfile.md)). A store of the sum, or
  of the register added into it, reads two and still fuses.

| Stage | `pbench kernel` | Doom `demo3` |
|---|---|---|
| The design's | 666 | 525 |
| … `+reads:2` | 667 | 525 |
| … `+last:br` | 676 | 531 |
| … `+reads:2+last:br` | 677 | 531 |
| … `+last:br:ld:st` | 696 | 583 |
| … `+fetch:4l+queue:16` | 669 | 525 |
| … `+reads:2+last:br+fetch:4l+queue:16` | 678 | 532 |

- **Branches last cost 10 / 6,** 1.5 % and 1.1 % of the issue cycles.
  Pairs fall from 162 to 152 per 1000 instructions on the kernel paths
  and from 206 to 200 on Doom.
- **Two reads an op cost under 1:** 0.6 / 0.8 fused stores per 1000
  instructions would read a third register.
- **Together they cost 11 / 7,** and 10 / 7 behind the line fetch the
  [front end](./front-end.md#design) designs.
- **Accesses last as well** (`last:br:ld:st`) would leave no op of a
  pair able to cancel the other, but cost 29 / 58: a load or store
  with an independent op behind it is among the commonest pairs, and
  Doom's loops are made of them ([ruled out](#ruled-out)).

## What a fused op pairs with

Measured 2026-11-22 behind gen3's TLB on the same windows, from an
image rebuilt since, on which the design's stage with branches last
and two reads an op,
`dual+fuse:lilui:cmpbr:movop:opbr:addmem:addwb+fw:imm+flex+reads:2+last:br`,
takes 681 / 533. Every other measurement here lets a fused op pair with
any op its slots allow. The model's `fpair:` restricts that:

- **An op that is not fused** (`fpair:simple`). X issues no fused op,
  so a fused op takes Y and its partner X, and two fused ops never
  pair.
- **Nothing** (`fpair:none`): a cycle issues one fused op, two ops, or
  one.

| A fused op pairs with | `pbench kernel` | Doom `demo3` |
|---|---|---|
| Any op | 681 | 533 |
| An op that is not fused | 706 | 551 |
| … without `flex` | 733 | 569 |
| Nothing | 718 | 602 |

- **A fused op alone costs 37 / 69,** 5.5 % / 12.9 % of the issue
  cycles: Doom loses 69 of its 240 pairs per 1000 instructions, the
  kernel paths 37 of 173. 29 % of Doom's ops are fused, against 17 %
  of theirs.
- **A partner that is not fused takes back 12 / 51 of it,** leaving
  it 25 / 18 behind any op. It is what a window of three instructions
  in ID buys ([structure](./structure.md#open)).
- **What it cannot serve is a neighbour that needs Y as well:** an
  access, a branch, or a second fused op. A load or store is the
  younger op of 8.5 of the 25 pairs per 1000 instructions it gives up on
  the kernel paths, and of 12 of the 18 on Doom.
- **`flex` is worth 26 / 18 here,** against 44–70 with branches free
  to lead ([fusion and dual issue](#fusion-and-dual-issue-together)),
  whose commonest case was a simple op behind a fused branch. What it
  serves now is a fused op that is the older of its pair, which pairs
  only from Y.

## Precise exceptions

An exception saves the PC of the instruction that faulted, and the
handler retries it by returning there
([entry sequence](../../system/architecture.md#entry-sequence)): every
instruction ahead of it has taken effect, and none from it on. A pair
has to keep that for its two ops, and a fused op for the two
instructions inside it.

### A pair

Loads and stores trap (a TLB miss, a protection or alignment fault, a
bus fault), as do `DIV` with a zero divisor and the system instructions
([instruction properties](../../system/instruction-set.md#instruction-properties)).

- **At most one op of a pair can trap.** X issues ALU ops, shifts and
  moves, none of which trap; a pair holds one memory access; system
  instructions and `DIV` issue alone.
- **The other op commits when it is the older.** gen3 settles a TLB,
  protection or alignment fault at MEM2, before any bus transaction
  and before an op writes anything
  ([fault precision](../penumbra3/memory-completion.md#fault-precision)),
  so the verdict gates the pair's register writes, its flag write and
  its store as a mask of two bits. MEM2 is the one place a pair splits.
- **A redirect never splits a pair.** A branch is always the younger
  op of its pair ([branches last](#branches-last-and-two-reads-an-op)),
  so the op beside a branch that resolves wrong commits, and the
  redirect flushes only what follows.
- **The flags' select stays a decode-time mux.** Loads and stores write
  no flags, so the op that traps is never the pair's youngest flag
  writer. `DIV` is the one op that both traps and writes the flags, and
  issuing it alone keeps it so; a partner would save one cycle of the
  tens it takes.
- **A bus fault arrives with the access's completion,** after MEM2. The
  pair holds in MEM2 while the access is pending, as gen3 holds an
  access and everything younger
  ([the pipe gate](../penumbra3/memory-completion.md#what-the-pipe-gate-freezes)),
  and leaves it with the mask applied.

### A fused access

The address fusions are the only ones that hold an instruction that can
trap, and it is always the second. `lilui`, `cmpbr`, `opbr` and
`movop` hold none: `movop` takes an ALU op or a shift, never a `MUL` or
`DIV`.

- **A fault in the access commits the add.** The fused op writes what
  the `ADD` alone would, its register and its flags, drops the load's
  write or the store, and saves the access's PC, four past its own.
  Stores, `addwb` and `incmem` write the sum and the flags in any case.
  The one new path is `addmem`'s load, which overwrites the add's
  register: on a fault that register takes the sum in place of the
  loaded word.
- **The handler returns to the access,** which runs unfused. The refill
  handler takes the faulting address from the MMU, never from the
  instruction.
- **A fetch fault stops fusion.** An instruction whose fetch faulted
  fuses with nothing. A fetch ends at its line, and so at its page, so
  only a pair carried from one fetch to the next can straddle one.
- **Interrupts come between ops,** never between the instructions of a
  fused op, and save the next op's PC.

Refetching unfused is the fallback if the overwrite costs a path: flush,
fetch again from the add with fusion held off for one op, and let the
access fault alone. It changes no datapath, but costs a redirect on
every fault in a fused access. With a software-managed TLB every refill
is one, and Doom fuses 73 accesses per 1000 instructions.

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
- **Fusing a shift pair into an extension** (`ext`): 0.7–1.1 per 1000
  instructions alone, and 0.1 issue cycles on the stage the front end
  measures ([measured](#address-and-shift-fusions)). Sub-word values
  arrive extended from `LDBS` and `LDHS`.
- **Fusing a scaled index into its add** (`shadd`): 2.7–8.0 per 1000
  instructions, under 3 issue cycles.
- **Two memory accesses in a pair** (`x:simple:ld+mem:ld2`, a load
  port in X), for gen4. It is worth 71 / 16 with all five address and
  shift fusions, the largest lever left on the kernel paths, but it
  takes a second data read of the TLB and of the L1-D. gen3 already
  keeps the TLB as two copies written together, one for fetch and one
  for data, and spends each L1-D column's second block-RAM port on
  fills and stores. A third copy and a second hit-and-permission
  verdict would sit in MEM2's verdict cone, gen3's timing-critical
  compare ([MEM2 verdict](../penumbra3/mem-stage.md#the-mem2-verdict-cone)).
  It is a step for after gen4, as gen2.5 followed gen2.
- **Saving the add's PC for a fault in a fused access,** with neither
  instruction taking effect. A retry from the add is correct, and
  NetBSD's fault handler takes the access type and address from the
  MMU rather than the instruction, but the saved PC would name an
  instruction that did not fault, against the ISA's contract
  ([precise exceptions](#a-fused-access)).
- **An op beside a branch that is not taken,** the branch the older of
  the two: 10 / 6 ([measured](#branches-last-and-two-reads-an-op)). The
  op would be cancelled when the branch resolves wrong in EX, a second
  place a pair splits beside MEM2's fault.
- **Accesses always the younger op as well** (`last:br:ld:st`), so that
  no op of a pair cancels the other: 29 / 58.
- **A third register read for a fused store,** a fifth read port or a
  rule that keeps such an op from a partner reading two: it would serve
  0.6 / 0.8 fused stores per 1000 instructions, worth under 1.
- **A fused op in X, or beside another fused op** (`fpair:all`, which
  the other measurements here assume): X would need every fused op's
  datapath, and ID a window of four instructions
  ([what a fused op pairs with](#what-a-fused-op-pairs-with)).

## Design

- **Fuse first.** The front end fuses `LLI`/`LUI`, an ALU op and the
  `Bcc` behind it, compares included, and a `MOV` with the op that
  updates its copy.
- **Fuse an add into the access it addresses** (`addmem`, `addwb`): a
  base-plus-index load or store, writing its address back when a load
  does not overwrite it. A two-input address adder takes 98–99 % of
  `addmem`'s accesses; a pre-incremented access (`incmem`) needs the
  offset as a third input. A fault in the access commits the add
  ([a fused access](#a-fused-access)).
- **Two reads an op.** An `ADD` of two registers fuses into no store of
  a third, so no op reads more than two registers, and a pair no more
  than the register file's four ports.
- **Two slots, either order, a branch last.** X issues simple ALU ops,
  shifts and moves; Y everything. Either op may be the older, but a
  branch is always the younger
  ([branches last](#branches-last-and-two-reads-an-op)).
- **A fused op takes Y,** beside an op that is not fused, and two fused
  ops never pair: 706 / 551 with branches last and two reads an op.
  Where its timing allows no partner, a fused op issues alone, at
  718 / 602 ([what a fused op pairs with](#what-a-fused-op-pairs-with)).
- **One forward between the pipelines:** a constant the older op loads,
  wherever the younger reads it.
- **The youngest flag writer's flags stand.**
- **One memory access a pair, in Y.** Two never pair: a second would
  double the TLB's and the L1-D's data reads ([ruled out](#ruled-out)).
  With X's ops unable to trap and `DIV` issuing alone, at most one op
  of a pair traps ([precise exceptions](#precise-exceptions)).
- **Pre-decode decides.** Fusing and pairing depend only on the
  instructions, so both are settled at the fetch FIFO's enqueue.

## Open

- **The address adder:** three inputs or two, and the `ADD`'s flags
  beside the address, to be timed in EX, ahead of the register the TLB
  and the cache read. Two inputs cost 11 / 7 issue cycles
  ([the address adder](#the-address-adder)).
- **The prologue's save at an offset.** A frame with a callee-saved
  register's slot at `[SP + 0]` would let a two-input adder fuse the
  frame's allocation with that save; the compiler places the slots.
- **A fused access behind a load.** Fusing an add into its access
  leaves a base that a load writes still waiting for the load, and the
  load-use model fuses nothing ([load-use](./load-use.md)).
- **Stalls.** A pair whose younger op waits on a load holds both ops,
  and the load-use distance decides how often.
- **Greedy pairing.** With `flex`, a store ahead of a compare takes the
  compare and leaves its branch alone. The cycle count is the same
  either way on these loops, but a rule that looks one op further might
  not be.
- **The fused compare-and-branch's path:** adder, flags, condition and
  resolve in one cycle, to be timed.
- **Other workloads.** The shell loop was not run.

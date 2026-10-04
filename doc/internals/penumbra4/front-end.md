# Penumbra/4 — The front end: fetch supply and the fuser's reach

> **Applies to:** Penumbra/4 · fetch, pre-decode and the fetch queue
> (ideas and measurements; nothing is built).

Two issue slots that each take a fused pair can use four instructions
in a cycle. This records how many the front end has to supply for the
issue stage of [issue-pairing](./issue-pairing.md) to run at its rate,
what the cycle after a taken transfer costs, and how far the fuser has
to see.

## Measurement setup

Measured 2026-10-04 on the two windows issue-pairing uses:
`pbench kernel` (720 M instructions) and Doom `demo3` (2.89 G). Figures
are cycles per 1000 instructions, as `pbench kernel` / Doom. The
fuser's reach is `+pairmodel`'s `reach:` modifier; the fetch simulation
is a scratch build of the ISS, not in the tree.

The issue stage is the design issue-pairing ends on: two slots with the
older op in either, the immediate forward, and four fusions (`LLI`
with `LUI`, a compare or other ALU op with the `Bcc` behind it, and
`MOV` with the op that updates its copy):
`dual+fuse:lilui:cmpbr:movop:opbr+fw:imm+flex`. With every instruction
always at hand it issues in 679 / 630 cycles.

The fetch simulation puts a fetch stage and a queue ahead of it:

- **A fetch** delivers up to its width in instructions, stopping at a
  taken transfer and, where stated, at the end of its 16-byte line. It
  runs only when the queue has room for a full one.
- **After a taken transfer** fetch idles for the bubble, one cycle
  where gen3 redirects from its pre-decode
  ([gen3 branch prediction](../penumbra3/branch-prediction.md)).
- **Issue** takes one op or a pair from what the queue held when the
  cycle began, by the pairing model's rules, fusing whatever two
  instructions sit next to each other in it. It waits for nothing that
  has not arrived.

With unlimited supply the simulation takes the pairing model's own
cycles. It leaves out mispredictions, which it charges only the bubble,
cache misses and the wait behind a load.

## What issue takes

Share of cycles by instructions issued, with every instruction at hand:

| Instructions in the cycle | `pbench kernel` | Doom `demo3` |
|---|---|---|
| 1 | 65 % | 58 % |
| 2 | 25 % | 26 % |
| 3 | 9 % | 15 % |
| 4 | 1.4 % | 1.0 % |

The average is 1.5–1.6 instructions a cycle. Four in one cycle takes
two fused pairs that also pair with each other, and is rare.

## Fetch supply

| Front end | Cycles | Over unlimited |
|---|---|---|
| Unlimited supply | 679 / 630 | — |
| 1 a cycle, bubble (gen3's front end) | 1,085 / 1,076 | 60–70 % |
| 2 a cycle within a line, bubble | 704 / 655 | 4 % |
| 2 a cycle across lines, bubble | 696 / 645 | 2–3 % |
| A line (up to 4), bubble, 8-entry queue | 693 / 635 | 1–2 % |
| A line (up to 4), bubble, 16-entry queue | 681 / 631 | under 0.5 % |
| 2 a cycle within a line, no bubble | 683 / 633 | 0.5 % |
| A line (up to 4), no bubble | 679 / 631 | none |

The two-a-cycle rows have an 8-entry queue.

- **Two a cycle is enough.** It is within 4 % of unlimited supply while
  paying a bubble on every taken transfer, and within 0.5 % without the
  bubble.
- **One a cycle is slower than single issue,** since nothing makes up
  the bubbles.
- **Width past two and the bubble are each worth 10–25 cycles per
  1000, and overlap.** A whole line into a 16-entry queue hides the
  bubble; so does removing it at two a cycle.
- **Doom's short loops are not the hard case.** Its pixel loops end in
  a taken branch every dozen or so instructions, but they are serial
  chains that pair little, so fetch has slack.

gen3's L1 keeps a line as one block RAM a word, and a lookup reads all
of them. The words a wider fetch delivers are read already; what it
adds is behind the read: a pre-decode a word and a queue that takes
more than one entry a cycle.

## The fuser's reach

A fetch of two delivers an aligned pair, and a fuser that looks only
inside each fetch misses every pair that straddles two. Three reaches
(`reach:any`, `reach:line`, `reach:pair`), with supply unlimited:

| Stage | Cycles |
|---|---|
| Fuses any two adjacent instructions | 679 / 630 |
| Only two in one 16-byte line | 737 / 672 |
| Only an aligned pair, the first at an even word | 760 / 687 |
| Aligned pair, with the same-cycle forwards behind it | 713 / 658 |
| No fusion, the same-cycle forwards | 733 / 723 |
| No fusion, the immediate forward alone | 815 / 791 |

The same-cycle forwards carry the older op's flags to a branch, a
copy's source and store data to the younger op
([pairing through forwards](./issue-pairing.md#pairing-through-forwards)).

Share of the fusable pairs an aligned fuser catches:

| | `pbench kernel` | Doom `demo3` |
|---|---|---|
| `LLI` with `LUI` | 25 % | 46 % |
| A compare with its `Bcc` | 47 % | 37 % |
| `MOV` with its op | 46 % | 69 % |
| Another ALU op with its `Bcc` | 30 % | 98 % |

- **An aligned fuser keeps 75–85 % of the saving over single issue**
  and gives up 56–81 cycles per 1000.
- **What it catches is the binary's layout, not chance.** A pair sits
  where its code was linked, and hot code decides the share. Doom's
  loop-ending pairs happen to sit on even words and nearly all are
  caught; the kernel paths' `LLI`/`LUI` pairs lean on the TLB refill
  handler, where most sit on odd ones.
- **A line is not enough either:** it misses up to 47 % of a kind of
  pair, where an even spread would miss a quarter.
- **Most of fusion's gain is the dependency it removes.** With the
  forwards behind it, the aligned fuser keeps 90 % of the saving: a
  missed pair that can still issue together loses little. What costs is
  a dependent pair that can do neither.

## Design

- **Fetch two instructions a cycle.** A whole line is a refinement
  worth 1–3 %.
- **The fuser sees every adjacent pair.** It carries the last
  instruction of a fetch over to the next, or fuses where instructions
  leave the queue. That reach is worth more than fetch width: 56–81
  cycles per 1000, against 10–20 from two a cycle to four.
- **Then the bubble,** by a deeper queue behind a wider fetch or by a
  redirect a cycle sooner.

An aligned fuser is the fallback if carrying an instruction over costs
the clock. The same-cycle forwards would then earn the place
[issue-pairing rules them out of](./issue-pairing.md#ruled-out).

## Open

- **Mispredictions and misses.** Each empties the queue, and refilling
  it favours a wider fetch; neither is in the simulation.
- **Waiting for a partner.** Issue here takes the op at the queue's
  head at once. Holding it a cycle for an instruction it could fuse
  with was not tried.
- **Two a cycle into a deeper queue** was not run; the 16-entry row is
  a whole line's.
- **The compiler's side of an aligned fuser.** Which word a pair starts
  on is a property of the code, so a pass could move pairs onto even
  words by swapping one with an independent neighbour.
- **A fetch stage in the pairing model.** `+pairmodel` has none; the
  fetch-supply figures came from a scratch build.

# Penumbra/4 — The front end: fetch supply and the fuser's reach

> **Applies to:** Penumbra/4 · fetch, pre-decode and the fetch queue
> (ideas and measurements; nothing is built).

Two issue slots that each take a fused pair can use four instructions
in a cycle. This records how many the front end has to supply for the
issue stage of [issue-pairing](./issue-pairing.md) to run at its rate,
what the cycle after a taken transfer costs, and how far the fuser has
to see.

## Measurement setup

Measured 2026-10-04 with `+pairmodel` on the two windows issue-pairing
uses: `pbench kernel` (720 M instructions) and Doom `demo3` (2.89 G).
Figures are cycles per 1000 instructions, as `pbench kernel` / Doom.
The fetch stage is the model's `fetch:`, `bubble:` and `queue:`
modifiers, and the fuser's reach its `reach:`.

The issue stage is the design issue-pairing ends on: two slots with the
older op in either, the immediate forward, and four fusions (`LLI`
with `LUI`, a compare or other ALU op with the `Bcc` behind it, and
`MOV` with the op that updates its copy):
`dual+fuse:lilui:cmpbr:movop:opbr+fw:imm+flex`. With every instruction
always at hand it issues in 679 / 630 cycles.

The model's fetch stage puts a queue ahead of issue and fills it:

- **A fetch** delivers up to its width in instructions, stopping behind
  a taken transfer, a trap or an interrupt and, where stated, at the
  end of its 16-byte line. It runs only when the queue has room for a
  full one.
- **After a taken transfer** fetch idles for the bubble, one cycle
  where gen3 redirects from its pre-decode
  ([gen3 branch prediction](../penumbra3/branch-prediction.md)).
- **Issue** takes one op or a pair from what the queue held when the
  cycle began, by the pairing model's rules, fusing whatever two
  instructions sit next to each other in it. It waits for nothing that
  has not arrived.

A fetch wider and a queue deeper than issue can use, with no bubble,
take the cycles the stage takes with every instruction at hand. The
model leaves out mispredictions, which it charges only the bubble,
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
two fused pairs that also pair with each other, and is rare. The
address fusions raise the average to 1.5–1.9
([behind the address fusions](#behind-the-address-fusions)).

## Fetch supply

| Front end | Modifiers | Cycles | Over unlimited |
|---|---|---|---|
| Unlimited supply | none | 679 / 630 | — |
| 1 a cycle (gen3's front end) | `fetch:1` | 1,085 / 1,076 | 60–70 % |
| 2 a cycle within a line | `fetch:2l` | 704 / 655 | 4 % |
| 2 a cycle across lines | `fetch:2` | 696 / 645 | 2–3 % |
| 2 a cycle within a line, 16-entry queue | `fetch:2l+queue:16` | 694 / 649 | 2–3 % |
| A line (up to 4) | `fetch:4l` | 693 / 635 | 1–2 % |
| A line (up to 4), 16-entry queue | `fetch:4l+queue:16` | 681 / 631 | under 0.5 % |
| 2 a cycle within a line, no bubble | `fetch:2l+bubble:0` | 683 / 633 | 0.5 % |
| A line (up to 4), no bubble | `fetch:4l+bubble:0` | 679 / 631 | none |

A row has the one-cycle bubble and an 8-entry queue unless it says
otherwise.

- **Two a cycle is enough for this stage.** It is within 4 % of
  unlimited supply while paying a bubble on every taken transfer, and
  within 0.5 % without the bubble. It is not once the address fusions
  are in ([behind the address fusions](#behind-the-address-fusions)).
- **One a cycle is slower than single issue,** since nothing makes up
  the bubbles.
- **Width past two and the bubble are each worth 10–25 cycles per
  1000, and overlap.** A whole line into a 16-entry queue hides the
  bubble; so does removing it at two a cycle.
- **A deeper queue does little for two a cycle:** 16 entries take back
  7–10 of the 25 cycles it loses. Where issue takes two or more
  instructions a cycle, a fetch of two cannot get ahead of it, whatever
  the queue could hold; where issue is slower, sixteen entries fill
  nearly as often as eight do.
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

## Behind the address fusions

Measured 2026-11-09 behind gen3's TLB (`+core=penumbra3`), on the
windows of [issue-pairing](./issue-pairing.md#address-and-shift-fusions)'s
address fusions. The stage above, behind that TLB, takes 674 / 616
with every instruction at hand, 698 / 633 at `fetch:2` and 683 / 621
at `fetch:4l`. The columns add to it `ext`, `shadd` and `addmem`, and
then `addwb` and `incmem` as well. Issue takes 1.50 / 1.85 instructions
a cycle with the first three and 1.53 / 1.92 with all five.

| Front end | Modifiers | Three more fusions | All five |
|---|---|---|---|
| Unlimited supply | none | 666 / 542 | 653 / 521 |
| 2 a cycle across lines | `fetch:2` | 692 / 602 | — |
| 2 a cycle within a line | `fetch:2l` | 705 / 616 | — |
| 2 a cycle across lines, 16-entry queue | `fetch:2+queue:16` | 683 / 598 | — |
| 2 a cycle across lines, no bubble | `fetch:2+bubble:0` | 672 / 546 | 660 / 538 |
| A line (up to 4) | `fetch:4l` | 675 / 559 | 664 / 539 |
| A line (up to 4), 16-entry queue | `fetch:4l+queue:16` | 668 / 542 | 656 / 522 |
| Up to 4 across lines | `fetch:4` | 667 / 542 | — |

A dash is a stage not measured.

- **Two a cycle loses 4.0 % / 11.1 %,** where without the address
  fusions it loses 3.5 % / 2.8 %. A stream that issues 1.85
  instructions a cycle empties the queue behind every taken transfer,
  92 / 81 of them per 1000 instructions, and a fetch of two cannot get
  ahead of it again. Sixteen entries take back 4–9 cycles of the
  27–60.
- **The bubble is the cost.** Without it, two a cycle is within 1 % with
  three more fusions, and within 1.0 % / 3.4 % with all five.
- **A line into a 16-entry queue is within 0.5 %** with all five,
  paying the bubble: a fetch of a line gets ahead of issue between
  taken transfers, and the queue holds what it gets.

## With a fused op in Y

Measured 2026-11-22 behind gen3's TLB, on the stage of
[issue-pairing](./issue-pairing.md#what-a-fused-op-pairs-with) where X
issues no fused op and two fused ops never pair:
`dual+fuse:lilui:cmpbr:movop:opbr:addmem:addwb+fw:imm+flex+reads:2+last:br+fpair:simple`.
Issue takes 1.42 / 1.81 instructions a cycle, against 1.47 / 1.88 where
a fused op pairs with any op.

| Front end | Modifiers | Cycles | Over unlimited |
|---|---|---|---|
| Unlimited supply | none | 706 / 551 | — |
| A line (up to 4), 16-entry queue | `fetch:4l+queue:16` | 707 / 551 | under 0.1 % |
| A line (up to 4) | `fetch:4l` | 709 / 552 | 0.4 % / 0.1 % |
| 2 a cycle across lines, no bubble | `fetch:2+bubble:0` | 707 / 562 | under 0.1 % / 2.0 % |
| 2 a cycle across lines | `fetch:2` | 718 / 611 | 1.6 % / 10.9 % |
| 2 a cycle within a line | `fetch:2l` | 726 / 651 | 2.7 % / 18.1 % |

- **A line into sixteen entries stays within 0.2 %** whatever a
  fused op pairs with: any op, one that is not fused, or nothing.
- **Eight entries are nearly enough.** A line into eight loses
  0.4 % / 0.1 %.
- **Two a cycle still pays the bubble on Doom:** 10.9 % with it, 2.0 %
  without. Where a fused op pairs with any op it loses 2.7 % / 13.0 %,
  and where it issues alone 1.2 % / 2.8 %. Fewer instructions a cycle
  leave fetch slack, but Doom still issues 1.81 a cycle here, and its
  loops empty the queue behind every taken transfer.

## Design

- **Fetch a line a cycle into a 16-entry queue.** With the address
  fusions, two a cycle loses 4–11 % and a line into sixteen entries
  under 0.5 %. Without them two a cycle is within 4 %, and a whole line
  a refinement worth 1–3 %. With a fused op in Y, eight entries are
  within 0.4 % ([with a fused op in Y](#with-a-fused-op-in-y)).
- **The fuser sees every adjacent pair.** It carries the last
  instruction of a fetch over to the next, or fuses where instructions
  leave the queue. That reach is worth more than fetch width: 56–81
  cycles per 1000, against 10–20 from two a cycle to four.
- **Or two a cycle and no bubble.** Two a cycle with a redirect in the
  cycle that fetches the transfer is within 1.0–3.4 %, and needs taken
  transfers predicted at fetch.

An aligned fuser is the fallback if carrying an instruction over costs
the clock. The same-cycle forwards would then earn the place
[issue-pairing rules them out of](./issue-pairing.md#ruled-out).

## Open

- **Room for the fuser without a stage.** Decode is on gen3's tightest
  fetch path: IF2 decodes each way's word as it leaves the block RAM
  and selects the hit's result into the fetch buffer
  ([gen3 timing outline](../penumbra3/timing-outline.md#1-front-end--mostly-proven-in-gen25-carry-it-forward)).
  A line a cycle needs four times the decoders, with the fuser and the
  pair marks behind them, and a stage ahead of the queue would add to
  every misprediction. Two places have room:
    - **The L1-I's fill,** which could store what depends on its line
      alone, an instruction's class and whether it fuses with the next
      word of the line, in the four bits a 32-bit word leaves free in a
      36-bit block-RAM row. A miss pays for it, and a hit does not.
    - **The queue's read side,** where pairing compares registered
      fields beside the scoreboard's compares rather than behind them.

  A probe of the front end decides, and the aligned fuser is the
  fallback.
- **Mispredictions and misses.** Each empties the queue, and refilling
  it favours a wider fetch; neither is in the model.
- **Waiting for a partner.** Issue here takes the op at the queue's
  head at once. Holding it a cycle for an instruction it could fuse
  with was not tried.
- **The compiler's side of an aligned fuser.** Which word a pair starts
  on is a property of the code, so a pass could move pairs onto even
  words by swapping one with an independent neighbour.

# Penumbra/3 — Branch prediction: measurements and design

> **Applies to:** Penumbra/3 · front end (in design).

gen3 resolves every control transfer in EX, and the front end can
redirect fetch earlier in two places: at fetch, from a branch target
buffer read beside IF1, and at the IF2 pre-decode, from the fetched word
itself. This records what each prediction component is worth, measured
on real workloads with the ISS's branch-predictor model, and the design
the numbers point to. It builds on the front-end rules in
[`timing-outline.md`](./timing-outline.md#1-front-end--mostly-proven-in-gen25-carry-it-forward).

## Measurement setup

Measured 2026-09-30 with the ISS's `+bpmodel` (`sw/sim/bp_model.h`),
which runs every control transfer through shadow predictors side by
side and counts where each would have redirected fetch to the right
path. The workloads are the shell loop, `pbench kernel` and Doom `demo3`
([workloads](./memory-performance.md#miss-profiles-from-the-shadow-models)).

A redirect is priced beyond the transfer's own issue cycle:

| Where | Cycles | Why |
|---|---|---|
| Fetch, from a target buffer | 1 | A block-RAM read beside IF1 answers in IF2: one discarded fetch, as gen2.5's predicted taken branch costs |
| IF2 pre-decode | 1 | The same stage; an estimate until its path is timed ([open](#open)) |
| EX | 4 | gen2.5's unpredicted taken branch ([instruction timing](../../system/instruction-timing.md#control-transfers)) |

The counts are exact for the architectural stream; the prices are not.
Tables train with each outcome as it executes, where hardware trains a
few cycles later, so tight loops read slightly optimistic.

## Where the transfers are

Per 1000 instructions:

| | shell loop | `pbench kernel` | Doom `demo3` |
|---|---|---|---|
| Conditional (taken) | 108 (42) | 107 (36) | 91 (57) |
| Direct (`B`) | 11 | 9.8 | 7.2 |
| Calls (`BL`) | 11 | 11 | 2.6 |
| Indirect calls (`JALR`) | 0.9 | 3.2 | 0.9 |
| Returns (`JMP R13`) | 12 | 14 | 3.6 |
| Other indirect (`JMP`) | 2.8 | 2.0 | 0.1 |

Conditional branches carry most of the cost: a direct transfer's
target is in its own word, but only a predictor knows whether a
conditional goes.

## Predictors compared

Branch-redirect cycles per 1000 instructions:

| Predictor | shell loop | `pbench kernel` | Doom `demo3` |
|---|---|---|---|
| None: every taken transfer resolves in EX | 321 | 300 | 286 |
| gen2.5's: a 32-entry buffer of direct transfers | 228 | 218 | 122 |
| IF2 pre-decode: direct targets, backward taken | 229 | 228 | 151 |
| … plus an 8-deep return stack | 194 | 187 | 141 |
| … plus a 1K-entry bimodal table | 122 | 111 | 91 |
| … plus a 256-entry buffer of indirect transfers | 115 | 96 | 88 |
| The same with a 1K gshare table | 139 | 116 | 97 |
| The same with a 4K gshare table | 120 | 100 | 94 |
| 1K-entry buffer, 4K gshare, 16-deep stack | 118 | 99 | 94 |

- **A small predictor recovers about 200 of the 290–320 cycles** that
  resolving everything in EX costs. Larger tables and a correlating
  predictor buy nothing more.
- **Each component's share, added in the order above:** pre-decode
  72–135, the direction table 50–76, the return stack 10–41, the
  indirect buffer 3–15.
- **Bimodal beats gshare** at 1K and matches it at 4K. The kernel's
  code footprint spreads a global history thin over the table, and
  bimodal needs no history register.
- **gen2.5's buffer does well on Doom** (122): its hot loops fit 32
  entries, and a resident branch predicts the direction it last went.
  On the kernel paths it leaves every return and most conditionals to
  EX.

## The return stack

Branch-redirect cycles per 1000 instructions, with pre-decode and a 1K
bimodal table:

| Return stack | shell loop | `pbench kernel` | Doom `demo3` |
|---|---|---|---|
| None | 157 | 157 | 101 |
| 4 deep, plain ring / counting | 124.6 / 124.6 | 117.8 / 117.8 | 91.0 / 91.1 |
| 8 deep, plain ring / counting | 122.1 / 122.1 | 114.1 / 114.1 | 90.8 / 90.8 |
| 16 deep, plain ring / counting | 121.4 / 121.4 | 113.6 / 113.6 | 90.6 / 90.6 |

- **A plain ring is enough.** A push overwrites the oldest entry, so
  the innermost returns — the ones worth predicting — stay predicted,
  and a stack desynced by `longjmp` or another return route keeps
  predicting instead of filling up and refusing. A pop predicts the top
  slot whether or not it still holds a return.
- **An empty check buys nothing:** a counting stack that predicts
  nothing once empty differs from the plain ring by at most 0.2 cycles
  per 1000. A return is always taken, so one predicted nothing and one
  predicted a stale slot pay the same redirect, and a stale slot is
  sometimes right.
- **Four entries take 90–95 % of what eight do,** and sixteen add
  under 1 cycle per 1000.

## The target buffer depends on the IF2 redirect

With the IF2 redirect at one cycle, the pre-decode already redirects
every direct transfer as early as a buffer read beside IF1 could, so a
buffer only adds indirect targets: 3–15 cycles per 1000. Two other
prices move it:

| Branch-redirect cycles per 1000 | shell loop | `pbench kernel` | Doom `demo3` |
|---|---|---|---|
| Pre-decode + bimodal + stack, IF2 redirect in 1 cycle | 122 | 111 | 91 |
| … IF2 redirect registered, 2 cycles | 194 | 178 | 159 |
| With a 256-entry indirect buffer, IF2 in 1 cycle | 115 | 96 | 88 |
| … IF2 redirect registered, 2 cycles | 134 | 113 | 91 |
| … and a buffer that redirects in 0 cycles | 59 | 41 | 22 |

- **If the IF2 redirect needs a register,** the buffer's one-cycle
  redirect saves 60–68 cycles per 1000 over pre-decode alone, and
  earns its place.
- **A 0-cycle buffer** saves another 55–66, but a block RAM read beside
  IF1 answers in IF2: it needs a buffer indexed by the fetch before,
  predicting the fetch after next.

## Why the gen2.5 estimate was higher

gen2.5's `flush` counter put 12–18 % of its kernel-path cycles in
front-end refill, about 350–500 per 1000 instructions. It counts every
redirect, including trap entry, `ERET` and each `WRSYS` resync, and on
the kernel paths those are mostly TLB-refill traffic, which the 256×2
TLB removes ([levers](./overview.md#levers-ranked-by-measured-benefit)).
Branches alone, under gen2.5's own predictor, come to 218–228 in the
model.

## What gen2.5's return stack taught

gen2.5 built an 8-deep ring and removed it with its ID-stage
backward-taken predictor in the fmax-closure round
([`hw-penumbra2.md`](../../TODO/hw-penumbra2.md), "gen2.5 status: RAS
landed"). The ring itself cost almost no clock. It redirected from ID,
and ID's redirect folded in the MEM stall, putting the data-cache tag
compare on the path into fetch; registering that redirect would have
fired in the cycle EX resolves the branch anyway. The removal entry
names what could have restored it: a stack predicting in the fetch
domain. gen3's pre-decode and return stack sit in IF2, ahead of the
fetch FIFO, where the back-end stall does not reach.

## Design

- **IF2 pre-decode redirects** direct transfers (`B`, `BL`, and `Bcc`
  the direction table calls taken), with the target from the word.
- **A 1K-entry bimodal table** of two-bit counters, read beside IF1 and
  used in IF2, trained off the fetch loop.
- **A plain return-stack ring,** 4–8 deep, pushed by calls and popped
  by returns in IF2, with no empty check.
- **A target buffer** only as the IF2 redirect's timing requires: a
  small indirect buffer when the redirect takes one cycle, a buffer
  over direct transfers too when it takes two.
- **EX verifies every prediction** and redirects on any mismatch, so no
  predictor structure has to be right, only cheap.

## Open

- **The IF2 redirect path.** It chains after the fetched word — tag
  compare, way select, pre-decode, then the stack's top or PC plus
  offset into the next PC — so it rides the IF2 cone
  [P0.5](./phase0-probes.md#p05--physical-tag-compare-behind-the-tlb-verdict-proposed)
  times. Whether it closes in the cycle decides between the two
  designs above.
- **The stream without refills.** The model ran under gen2.5's 32×2
  TLB, so the refill handler's branches are in the counts; a 256×2 TLB
  takes most of them out.

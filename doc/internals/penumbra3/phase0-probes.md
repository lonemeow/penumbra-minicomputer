# Penumbra/3 — Phase 0: structural feasibility probes

> **Applies to:** Penumbra/3 · pre-RTL feasibility gate.

Phase 0 of the [gen3 build plan](./overview.md#build-plan) proves that the
four load-bearing structural claims close at the target clock **before** any
real core RTL is built on them. It is the direct countermeasure to the gen2
dead-end, where combinational boundaries were wired up as "register later,"
the machine was built on top, and only at closure did one boundary turn out
to be a *structural* floor that could not be fixed without rewriting the
pipeline depending on it.

This document specifies each probe: the claim, the RTL to build, the exact
path to measure, the pass threshold, and the decision to take on failure.

## What a probe proves — and what it does not

A Phase-0 probe is **not** a bare cone, and it is **not** a fmax measurement
of gen3. It is a **kill-criterion** on a single registered boundary.

- **Necessary, not sufficient.** A boundary cone that fails its probe is
  structurally dead — no amount of later floorplanning recovers a
  combinational path that does not fit. A boundary that *passes* is only
  *plausible*; the authoritative fmax is the Phase-5 full-top read. The
  probe's job is to catch structural impossibility early, not to predict the
  final clock.
- **Why it is more trustworthy than gen2's probe.** gen2's
  `VARIANT=probe` measured a whole core's worst-of-all-paths with the system
  fabric omitted (~55 MHz probe vs ~31 MHz real) — the omitted fabric paths
  became the real limiter. A Phase-0 probe instead measures **one registered
  flop→logic→flop cone**, which is placement-bounded by construction. The
  claim is local, so the over-report is bounded.
- **Bounded, not zero.** A probe still places more compactly than the
  congested full design, so it under-represents routing. The
  [margin policy](#pass-bar-and-margin-policy) is what bridges probe→full.

## How a probe is built

Each probe is a small synthesizable top that instantiates the **real gen3
modules** straddling the boundary (these are the first gen3 RTL — minimal
versions that become the skeleton's modules), wired so that:

1. **Both endpoints are real flops.** The boundary under test is a register;
   the cone is flop→logic→flop. This is the property being proven.
2. **The consumer side carries realistic fanout.** Not a single comparator —
   the full-width structure the real design routes: the whole scoreboard
   read, the N-way tag compare + way-mux, every ID/EX flop the issue enable
   gates. Compact fanout is the single biggest source of probe over-report.
3. **`keep_hierarchy` is on** (confirmed to help placement realism in this
   flow), and module boundaries match the intended real hierarchy so the
   placer faces the same module-to-module routing.
4. **No unrelated logic shares the period.** The probe top contains only the
   boundary cluster plus the flops needed for realistic fanout — so the
   reported critical path *is* the cone under test, not an artifact.

## How a probe is read

- **Read the specific path, not the headline fmax.** `make timing` prints
  the top-N critical paths; identify the one that is the boundary cone and
  read *its* slack and its logic/routing breakdown.
- **Confirm the path against the RTL.** nextpnr labels fused LUTs by
  net-name prefix, not dataflow — "the path goes through X" can mean a LUT
  merely *named* X (an unrelated registered signal). Trace it to the actual
  modules before trusting it.
- **Prefer the logic-delay budget over a raw fmax ratio.** Routing in a
  probe is optimistic; logic delay is not. A cone whose *logic* portion
  already eats most of the period will never close once realistic routing is
  added, regardless of the probe's headline number.

## The harness

Each timing probe is an **integration variant** top under
`hw/rtl/fpga/ulx3s/`, named greppably and registered in the Makefile
`FPGA_TOPS` table so an invalid tuple is a hard error:

```
ulx3s_penumbra3_probe_issue_top.sv    (P0.1)
ulx3s_penumbra3_probe_decode_top.sv   (P0.2)
ulx3s_penumbra3_probe_memtlb_top.sv   (P0.3)
ulx3s_penumbra3_probe_l1tag_top.sv    (P0.5)
```

P0.4's question is functional, so it is a testbench
(`penumbra3_bus_master_test`) rather than a top.

Each composes `SRC_COMMON` + the gen3 probe modules (`SRC_CORE_penumbra3`,
filtered to the boundary cluster) + the board shell — but **not**
`SRC_FABRIC` (these are core-boundary probes; the bus fabric is stubbed,
except P0.4 which attaches the real SDRAM stack). Build and read with:

```sh
make fpga   BOARD=ulx3s CORE=penumbra3 VARIANT=probe_<name>   # synth + PnR
make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_<name> TOP_N=10
```

The clock constraint is **20 ns (50 MHz)** so slack is read directly against
the target. P0.4 additionally runs a **dual-clock functional sim** (the
existing `i_sdram_clk` at 4× convention) against the real `io/sdram` stack —
its primary question is functional (deadlock), not timing.

## Pass bar and margin policy

The target is 37.5 MHz on the ECP5; the 50 MHz budgets below are a margin
heuristic, not a gate. They exist so that no cone is already marginal at
37.5 MHz before the rest of the machine and its peripherals share the die.
A probe that misses them is read as slack at 37.5 MHz (26.7 ns): a few
percent is marginal and gets fixed before the design builds on it; around
20 % is not.

A probe **passes** only with margin, because it under-represents routing:

- **Non-BRAM cones (P0.1, P0.2):** the probed path closes at 50 MHz **with
  ≥30 % timing margin** — i.e. it also closes at ~65 MHz (~15.4 ns) with
  positive slack — *and* its logic-only delay leaves clear room for routing
  (target logic ≤ ~9 ns of the 20 ns period).
- **BRAM-output cone (P0.3):** after the **5.8 ns NOREG BRAM Tco** (a −6
  hardware floor), the remaining tag/permission logic + routing fits in the
  outline's **~14 ns budget** with positive slack at 50 MHz.
- **P0.4:** functional — read-fill *and* eviction-write bursts stream to
  completion in dual-clock sim with **no hang**; the registered boundary
  cone itself closes trivially (it is shallow).

The 30 % figure is a starting bar to be **calibrated** against the first
full-top read in Phase 1: once the skeleton gives a real probe-to-full
ratio for *this* design, later probes can trust a tighter or looser margin
accordingly.

## The probes

### P0.1 — registered load-completion back-end

**Claim.** The pipeline gate is a registered `load_pending` flop, not the
live cache `busy`; the gen2 floor cones — `cache busy → issue` and
`cache busy → MMU verdict`, the same cone with different tails — do not
exist.

**Build.**
- A one-entry load-hold buffer + a `load_pending` flop, set by a registered
  completion event delivering `{data | fault}`.
- The issue-side consumer at realistic width: the full scoreboard (~21
  valid bits) read for all decode source operands, the hazard/forward
  compare, `can_issue`, fanning out to every ID/EX flop enable.
- The cache `busy`/hit verdict wired **only** into the completion FSM whose
  output is the registered `load_pending` — never into `can_issue`.

**Path to read.** `load_pending` (and scoreboard flops) → `can_issue` →
ID/EX enable. This must be the longest path.

**Structural check (as important as the timing).** Prove by construction
that **no combinational arc exists from a `cache_busy` input to any
issue/enable output** — assert it (Verilator `--assert`) and confirm the
timing report shows the longest path originating at a flop, not at the
`busy` input.

**Pass.** Non-BRAM bar above. **On fail:** the registered-completion model
is the single highest-value gen3 change; if its cone cannot close, the whole
approach is in question — escalate, do not patch.

### P0.2 — registered decode→issue boundary

**Claim.** The fetch FIFO presents a *pre-decoded bundle*; ID's cone starts
from registered fields, and the hazard/forward compare runs on
already-registered register numbers (never a long path).

**Build.**
- The fetch FIFO storing the decoded control bundle (wide slot), written at
  enqueue, read registered.
- **Enqueue side:** the heavy word→bundle decode (port `penumbra2_decode`,
  incl. the CCU2 carry-chain op-decode) on the IF2→push path — included to
  show it fits the fetch cycle's slack.
- **ID read side:** regmap (arch→phys, on live `i_supervisor`), scoreboard
  lookup, forward-availability compare, `can_issue`.

**Path to read.** Two paths: (a) FIFO registered output → regmap →
scoreboard/forward compare → `can_issue` → enable (the claim); (b) the
enqueue decode → FIFO write (must fit the fetch cycle, which has slack).

**Tail check.** The issue/enable term must be **local** to the block owning
the ID/EX flops — verify it does not route ID→spine→ID (the gen2.5 seed
crossed that boundary twice on the enable cone).

**Pass.** Non-BRAM bar. **On fail:** the documented fallback is a real
DEC→ISSUE pipeline split (+1 mispredict/load-use cycle) — decide here
whether to take the stage rather than discovering the need mid-Phase-1.

### P0.3 — split MEM + duplicated BRAM TLB

**Claim.** MEM1 launches the cache index + the (duplicated, D-copy) BRAM TLB
read; MEM2 resolves cache data + tag compare + TLB verdict on the
*registered* BRAM output, all inside the period — earning back the BRAM TLB
gen2 had to revert to async LUTRAM for lack of a stage to absorb the sync
read.

**Build.**
- MEM1: EA registered from EX → cache index address + TLB BRAM read address.
- The D-copy BRAM TLB (1 read + 1 write/sysreg port = one DP16KD), sync
  registered read.
- MEM2: BRAM output → N-way tag compare + way-mux + TLB hit-detect +
  permission cone (R/W/X) → registered verdict.

**Path to read.** BRAM registered output → tag/permission compare → verdict
flop. Confirm the 5.8 ns Tco is accounted as BRAM clk-to-Q, and the
remaining logic+routing fits the ~14 ns budget.

**Pass.** BRAM-cone bar. **On fail:** the split-MEM/BRAM-TLB
interlock is what makes loads pipeline *and* the TLB sync-readable; if it
cannot close, reconsider TLB associativity or revert to async-LUTRAM TLB
(accepting its cone) — a structural call to make now, not later.

### P0.4 — CPU↔bus registered burst + SDRAM-adapter survival

**Claim.** A registered line-transaction (burst request out, streamed
response back through a fill buffer) into the **existing
`sdram_bus_adapter`** streams without the gapped-read deadlock — for
read-fills **and** write-back eviction bursts.

This is the probe that directly de-risks the one known sharp edge: in gen2,
registering the L2↔bus boundary turned a line fill into gapped single reads
and the adapter's speculative-prefetch FSM deadlocked (`test_l2_ifetch`
hung). A write-back L2 adds *eviction* bursts on the same path, so both
directions are covered now — otherwise an eviction-burst deadlock surfaces
late, a smaller replay of the same trap.

**Build.**
- The registered bus-master boundary: request register (line addr + r/w +
  burst length) and a response fill buffer.
- Attached to the **real** `io/sdram` stack (`sdram_bus_adapter` →
  `sdram_cdc` → `sdram_ctrl` → `sdram_phy_sim`) in a dual-clock testbench
  (sys + `i_sdram_clk` at 4×).
- A driver issuing back-to-back read-fill line bursts and back-to-back
  eviction-write line bursts.

**Pass.** Functional bar above (no hang, both directions); the boundary cone
closes trivially. **On fail (deadlock):** decide **now** — fix the shared
`sdram_bus_adapter` (it is `io/`, shared, so a fix serves all generations)
or change the gen3 burst pattern so the adapter never sees a gapped stream.
Never a "work around in the core later."

### P0.5 — physical-tag compare behind the TLB verdict

Added after the other four passed. P0.3 measured the TLB's verdict alone;
the L1 compares its tags against the frame that verdict selects, in the
same cycle, and the L1's size target sets how wide that compare is. See
[the L1 compare behind the verdict](./mem-stage.md#the-l1-compare-behind-the-verdict).

**Claim.** MEM2 (and IF2) close with the L1's physical-tag compare, at the
associativity the board's L1 size needs, chained behind the BRAM TLB
verdict; on the fetch side, the way-muxed word still reaches the
decode-on-enqueue FIFO write in the cycle, and the pre-decode's redirect
still reaches the next fetch PC.

**Build.**
- The real MMU: both BRAM TLB copies, the pinned TLB and bypass, each port
  exporting its frame candidates — the bypass-or-pinned frame, registered
  at launch, and each main-TLB way's — with a one-hot select.
- The L1's per-way tag BRAMs and word-column data BRAMs (a line written
  in one cycle, a store writing one column's bytes), registered, at the
  ECP5's 8 KB 2-way on both sides.
- The compare in the shape the design intends — each cache way against
  every candidate frame, the select picking — then, for MEM2, the way mux
  into the real MEM2 stage's sub-word extract, MEM2/WB register and
  load-completion buffer, and a registered store write. For IF2, the way
  select of bundles each way decoded from its own word into the real
  fetch FIFO, and the pre-decode redirect: the transfer's kind from each
  way's word, the direction counter, the return ring's top or PC plus
  offset, and the next-PC mux
  ([branch prediction](./branch-prediction.md#design)). The fetch control
  replays a miss rather than holding IF1 on it.
- Read/write exclusivity written into every array's RTL, so the netlist
  carries no read-during-write bypass after the EBRs.

**Path to read.** TLB BRAM output → match / candidate compares → hit and
way select → data way mux → result flop (MEM2), or FIFO write and the
pre-decode redirect into the fetch PC (IF2). A redirect that does not
close in the cycle takes a register, and costs a cycle on every predicted
taken transfer.

**Pass.** BRAM-cone bar. **On fail:** take the associativity down with the
L1 size (at the line-wide fill's cost 8 KB 2-way keeps 39–55 % of what
16 KB 4-way saves over 4 KB, and most of the remainder is on the fetch
side, so MEM2 fails more cheaply than IF2), or move the way select a
cycle later — a structural choice for the board's L1, made before the
L1 is written.

**Result** (2026-09-30, `ulx3s_penumbra3_probe_l1tag_top`, seeds 1–5):
42.5, 44.9, 43.4, 44.9 and 43.6 MHz — 12–17 % slack at 37.5 MHz, short of
the 50 MHz budget. The two sides take turns as the limit, each the cone
under test: MEM2's is 5.6 ns of Tco, about 7.5 of TLB match, 2 of
candidate compare and way select, then the sub-word extract and the
result mux into MEM2/WB; IF2's runs the same compare into the predicted
redirect and the PC and IF1/IF2 register enables. The candidate compare
costs about 2 ns after the TLB match. On the Nexys Video's Artix-7 (out of
context, Vivado 2025.2) the same probe closes at 86 MHz, its worst path
the IF2 enqueue in 9 logic levels, with every memory in block RAM: 29
RAMB18, 3.9K LUTs, 3.2K flip-flops.

The L1 data is in word columns, which both tools build as block RAM.
Byte lanes read a word at a time, with the select inside the EBR, closed
at 44.3–49.6 MHz on the same seeds for about 410 fewer LUTs, but Vivado
built them from registers (93K LUTs and 129K flip-flops for the probe),
since a store writes one word of a lane's row. Getting to either took
three changes to what the first build measured at 39.7–40.1 MHz:

- **Decode per way.** Decoding the way-muxed word left about 12 LUT levels
  of decode behind the TLB match and the compare (24.9 ns). Each way now
  decodes its own word beside them and the hit selects a bundle, for
  about 220 LUTs.
- **The redirect ignores the fault.** Qualifying it with the translation
  fault and the cacheable hit put P0.3's whole permission cone in front of
  the PC. A faulting slot traps and a missing one replays, both from
  registered state, so the redirect needs only a hit and FIFO room; the
  return stack, which must not take a wrong-path push or pop, updates a
  cycle after its transfer enqueues.
- **MEM2/WB's enable leaves the verdict.** Gating the whole payload
  register's load on the commit decision put the permission check,
  cacheability and the hit on about 350 enables. Only the valid bit now
  carries the commit.

A miss holding IF1 in the cycle it resolves, rather than replaying, never
became the worst path, so the replay contract's timing benefit is not yet
shown. Yosys's read-during-write emulation on the L1-D against
`no_rw_check` did not move fmax outside the seed spread; it costs about 600
flops and 300 LUTs.

## Recommended order — fail fast

The probes are independent and can run in parallel, but if run in sequence,
front-load risk:

1. **P0.4** first — it tests a *known* failure (the adapter deadlock) and a
   shared module a fix would ripple through; learn the worst news earliest.
2. **P0.3** — the BRAM TLB is the thing gen2 already failed to fit; prove
   the split-MEM stage actually earns it back.
3. **P0.1**, then **P0.2** — the load-completion and decode→issue cones;
   high value but lowest structural uncertainty (every surveyed core proves
   the shapes close on comparable fabric).

## Exit → Phase 1

Phase 0 exits when all four probes pass their bars, or when a failure has
been resolved by an explicit structural decision (boundary redesign, an
accepted extra stage, or a shared-module fix) — **not** by deferral.

Phase 1's first checkpoint is then the real composition proof: a **full-top
timing read of the bare composed skeleton** (the registered boundaries wired
together, before BTB/extra forwarding paths) — read on the full top, never a
probe variant. That is where the boundaries are proven to *compose* without
re-creating a spanning cone, and where the probe-to-full margin gets
calibrated for the rest of the project.

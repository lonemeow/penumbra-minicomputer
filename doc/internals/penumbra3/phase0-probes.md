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
ulx3s_penumbra3_probe_loadcomplete_top.sv   (P0.1)
ulx3s_penumbra3_probe_decodeissue_top.sv    (P0.2)
ulx3s_penumbra3_probe_memtlb_top.sv         (P0.3)
ulx3s_penumbra3_probe_busburst_top.sv       (P0.4, timing portion)
```

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
hung). Write-back (Phase 4) adds *eviction* bursts on the same path, so both
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

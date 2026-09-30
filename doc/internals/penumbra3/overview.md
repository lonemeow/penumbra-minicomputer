# Penumbra/3 — Overview & Build Plan

> **Applies to:** Penumbra/3 · pipelined in-order core (in design).

Penumbra/3 is a **fresh redo of the gen2 in-order pipeline**,
re-architected to clear the timing floor that capped gen2. It is **not**
superscalar and **not** out-of-order — it is the same single-issue,
in-order, classic-RISC machine, rebuilt so that every inter-block
boundary is a registered handshake and no cross-module combinational cone
survives to become a structural ceiling.

This document is the entry point for gen3: what it is, why it exists, the
fork boundary against the shared system, the pipeline shape, the feature
set, and — most importantly — the **build plan**, whose ordering is the
direct lesson learned from how gen2 dead-ended.

Companion: [`timing-outline.md`](./timing-outline.md) is the
structural-rules research (Rule Zero and the per-subsystem design points)
this plan is built on. Read it alongside this document; this overview does
not restate its rules in full.

## Why a third generation

gen2/2.5 is [abandoned](./../penumbra2/overview.md). It reaches **37.5 MHz**
on the ULX3S ECP5 only after a closure round stripped it back, with no
headroom left for its own remaining features. The limiter is not placement
— it is the **D-side memory-hit cone**: an in-order pure-stall machine
requires the cache hit verdict to gate commit *the same cycle*, so
`D-cache tag compare → busy → stall network → commit` is one combinational
path spanning the die (~29 ns, ~34 MHz floor), and the same cone reappears
on the MMU verdict and the load-use freeze. Floorplanning cannot shorten a
cross-module combinational path.

**The gen2 mistake gen3 must not repeat.** gen2 wired those boundaries
combinationally as "register them later," built the whole machine on top,
and only discovered at closure that the cone was a *structural* floor —
unfixable without rewriting the pipeline that now depended on it. The
microarchitecture to fix it already exists (the timing outline). What gen3
adds is **discipline of sequencing**: prove the hard structural claims hold
at the target clock, on realistic context, *before* building on them. That
discipline is the [build plan](#build-plan) below.

## What gen3 is — and is not

| | |
|---|---|
| **Is** | Single-issue, in-order, 7-stage pipelined RISC. Same ISA, ABI, exception model, and bus contracts as gen1/gen2. Boots the same NetBSD binary (CPUID-discriminated). |
| **Is not** | Superscalar, out-of-order, multi-issue, or speculative beyond fetch-time branch prediction. Those stay out of scope (a hypothetical gen4's territory). |
| **Target** | A **reliable** 37.5 MHz on the −6 ULX3S ECP5: the full top closes across a seed sweep, with headroom left for the staged features. 50 MHz is not a goal; a board with faster fabric (the Artix-7 Nexys Video) runs the same RTL at whatever its full top closes at. Performance comes from cycles — memory latency first, then prediction ([feature set](#feature-set-whats-foundational-vs-staged)). |

The discrete-logic constraint does **not** apply: like gen2, gen3 is
FPGA-only (BRAM caches, wide registered datapaths). Only gen1 and the ISA
honor 74xx feasibility. What gen3 still honors is the externally-visible
ISA contracts — bus protocol, autoconfig, the device sysreg interface —
because those are ISA-level, shared by every generation.

## The four structural changes (the thesis)

From the [timing outline](./timing-outline.md); everything else is
hygiene around these:

1. **Registered load-completion back-end.** The pipeline gate moves from
   the live cache `busy` to a registered `load_pending` flop; a miss parks
   the load in a one-entry hold buffer and completes on a registered event
   (`{data}` or `{fault}`). This deletes the gen2 floor cone — both the
   load-use freeze and the MMU-verdict path, which are the same cone.
   Faults stay precise by the in-order single-outstanding invariant.
2. **Registered decode→issue boundary.** The fetch FIFO presents a
   *pre-decoded control bundle*, not a raw word — the heavy word→bundle
   decode runs at FIFO enqueue (fetch-domain slack), so ID's cone starts
   from registered fields. The hazard/forward compare then runs on
   already-registered register numbers (the shape every surveyed core uses;
   never a 25 ns path). Regmap stays in ID on live supervisor mode.
3. **CPU↔bus transaction decouple (Amiga model).** A registered
   line-transaction request out, streamed response back through a fill
   buffer; the cache hides bus latency and the CPU clock is independent of
   bus speed. Memory stays a first-class autoconfig bus device — the async
   4-phase protocol is preserved *for peripherals*.
4. **Split MEM1/MEM2 + duplicated BRAM TLB + L2 burst fill.** Pipelines
   loads (kills gen2's ~0.35 CPI per-op bubble), gives the sync BRAM TLB
   the cycle it needs (launch in MEM1, verdict in MEM2), and makes L2 fills
   cost per-line not per-word — all interlocking.

## Pipeline shape

7 stages; pre-decode folds into the fetch-FIFO enqueue (no extra stage).

```mermaid
graph LR
    IF1[IF1<br/>PC→cache addr<br/>BTB+BHT read] --> IF2[IF2<br/>I-cache word<br/>predecode→FIFO]
    IF2 --> FIFO{{fetch FIFO<br/>decoded bundle}}
    FIFO --> ID[ID<br/>regmap, scoreboard/<br/>forward, issue]
    ID --> EX[EX<br/>ALU, flags,<br/>branch resolve,<br/>divmul launch]
    EX --> MEM1[MEM1<br/>EA→cache index<br/>+ TLB BRAM launch]
    MEM1 --> MEM2[MEM2<br/>cache data+tag,<br/>TLB verdict,<br/>load-completion]
    MEM2 --> WB[WB<br/>regfile/SR/SPR<br/>write, retire]
```

| Property | gen3 |
|---|---|
| Stages | 7 (IF1, IF2, ID, EX, MEM1, MEM2, WB) + elastic fetch FIFO |
| Hazards | Full operand forwarding (EX→EX, MEM2→EX) + NZCV flag forwarding; scoreboard interlock for the residual cases |
| Branches | Resolve in EX; fetch-time BTB → 0 bubbles on correct taken-prediction; cold/mispredict = registered flush |
| Load-use | ≈1–2 cycles with MEM2→EX forwarding (pinned in the pipeline-stages doc) |
| Loads | Single-outstanding, hold-and-complete (no per-op MEM bubble) |
| divmul | Side FU (reused `common/divmul.sv`), registered completion, dual-write sequenced over two WB cycles |
| Serializing ops (WRSYS/ERET) | Serialize + registered flush/redirect at commit; default serialize *every* WRSYS |

## Fork boundary

gen3 is a full `CORE=penumbra3` generation (like gen2 forked from gen1),
**not** a `penumbra2_5`-style composition sub-variant. It gets its own core
*and its own on-chip memory hierarchy*, because registering every
inter-block boundary is the whole thesis — and those boundaries are exactly
the L1/MMU/arbiter/fill/L2 modules gen2 *shared*. gen1 and gen2 stay
byte-frozen and re-synthesizable; gen3 only adds files.

The fork is **deeper than gen2's**: gen2 forked only the pipeline + L1 and
shared L2/MMU storage; gen3 forks the entire machine guts down to the
registered bus master. The seam is the **machine layer** — `machine_penumbra3`
keeps `machine_penumbra2`'s external port list byte-identical (a generic
master bus + IRQ + trace + program-end), so the board top is a near-verbatim
copy and the board never learns gen3 exists.

| Disposition | Modules |
|---|---|
| **Reuse as-is (generation-neutral leaf)** | `common/penumbra_pkg.sv` (ISA/system contract), `common/divmul.sv` (MUL/DIV peer FU), `common/cond_eval.sv`, `common/byte_ext.sv`, `common/byte_rep.sv`. `common/tlb_pinned.sv` = reuse-candidate, revisit under the MMU rewrite. |
| **Reuse as-is (bus devices + board)** | `soc/`: `boot_rom`, `busctl`, `cpuid`, `machid`, `timer`, `autoconfig_dev`, `bus_devsel`, `cache_perfctr`. All of `io/` (UART, SPI, **SDRAM controller stack**, video). The `fpga/ulx3s` PLL + shell. |
| **Fork & rewrite → `penumbra3/` (pipeline)** | core, spine, IF1/IF2/fetch_buffer/ID/EX/MEM1/MEM2/WB, decode, regmap, alu, regfile, scoreboard, flag_bypass, spr_file, scratch_file, irq, vecfetch, perfctr, pkg |
| **Fork & rewrite → `penumbra3/` (memory hierarchy — the boundaries Rule Zero registers)** | MMU + TLB storage (duplicated BRAM I/D, MEM1-launch/MEM2-verdict), L1 (write-through, hit→registered-replay), arbiter + CPU↔bus decouple (registered line-transaction), fill sequencer (burst), L2 (`soc/l2_cache.sv` → `penumbra3_l2.sv`; **write-through; write-back/write-allocate a measure-driven option — outside the core cones**) |
| **New thin wrappers** | `machine/machine_penumbra3.sv` (port list identical to gen2), `fpga/ulx3s/ulx3s_penumbra3_top.sv` (copy of gen2 top, swap the machine instance) |

**Known sharp edge.** Registering the L2↔bus boundary in gen2 tripped a
suspected **SDRAM-adapter deadlock on gapped reads** (`test_l2_ifetch`
hung). gen3's registered burst fill is the centerpiece, so it must issue
clean back-to-back bursts the existing `sdram_bus_adapter` tolerates, or
that adapter takes a (shared) fix. This is the one place "reuse `io/`
as-is" carries a real risk — and it is a Phase-0 probe.

## Feature set: what's foundational vs staged

**Foundational — designed in from the start.** Forwarding and the BTB are
*not* additive: the gen2 post-mortem is explicit that forwarding + branch
prediction reshape the EX/MEM/stall/commit region, which is exactly the
floor. Designing that region without them means optimizing a tail that gets
rewritten. So the Phase-1 skeleton already carries full operand forwarding,
flag forwarding, and the registered load-completion back-end; the BTB lands
right behind it.

**Shapes — built into the memory hierarchy as it is written.** The
largest measured costs are TLB refills and L1 fills
([memory-performance](./memory-performance.md)), and what removes them
is the shape of storage the L1, L2 and TLB fix when they are written.
Changing a shape later means rewriting the module, so these land with
the Phase-1 memory hierarchy:

- **The line-wide L2→L1 transfer with fill-through, in one uncore
  pipeline.** One L2 access returns the whole line and L1 installs it
  in a cycle; on an L2 miss, L1 takes the line the bus delivered. An
  L2-hit fill drops from 16 cycles to ~5 and an L2-miss fill from 37
  to ~24 ([the line-wide
  transfer](./memory-performance.md#the-line-wide-transfer)). The L2's
  first stage accepts a lookup every cycle, which the L1-I prediction
  buffer below depends on.
- **The main TLB at 256×2.** It removes 92–98 % of refills, the
  largest single cost measured, and costs no block RAM over 32×2
  ([TLB sets](./memory-performance.md#tlb-sets-and-a-cheaper-refill)).
  It needs `MMU_CONFIG` version 2 with invalidation done in the MMU
  ([timing outline](./timing-outline.md#5-mmu--tlb)); without it, the
  kernel's invalidation sweep grows with the TLB.
- **The L1 at 8 KB 2-way on the ECP5,** which the line-wide array
  shape gives for the block RAM of 4 KB; an Artix-7 board takes
  16 KB ([targets](#performance-targets)).
- **Read/write exclusivity written into every array,** so no BRAM
  output carries a synthesized bypass.

**Staged — additive, outside the core cones,** in the order the
measurements rank them. None reshapes the core's critical cones, so
each follows the booting skeleton:

1. **Store buffer** — the store-traffic strategy below.
2. **Next-line prediction buffer for L1-I** — most of what a 16 KB
   4-way L1-I would save on the kernel paths, with no change to the
   hit logic
   ([prediction buffer](./memory-performance.md#next-line-prediction-buffer-for-l1-i)).
3. **Return and direction prediction** — a RAS from the IF2
   pre-decode, and a direction table beside the BTB, each chosen by a
   predictor model in the ISS before it is built. Branches resolve in
   EX as on gen2, so a misprediction costs about what it did there,
   but its share of the cycles grows as the base CPI falls.
4. **Selective `WRSYS` serialization,** re-measured once the TLB has
   grown: a 256×2 TLB removes most of the refills whose `WRSYS`es make
   it pay ([timing outline](./timing-outline.md#multi-cycle-and-serializing-instructions-divmul-wrsys-eret)).
5. **Stream prefetch beside the L2,** for the streaming demos and bulk
   copies
   ([stream buffers](./memory-performance.md#next-line-prefetch-into-stream-buffers)).
6. **Write combining in the SDRAM adapter** — whole lines drained as
   one burst, inside the bus contract
   ([write combining](./memory-performance.md#sdram-side-write-combining)).
7. **Write-back L2,** only if stores prove throughput-bound.
8. **A native port for on-board RAM** — stretch: over write combining
   it adds an estimated ~3 cycles per 1000 instructions on the profiled
   workloads.

The store buffer needs no reserved seam in the core's cones: it sits
between the completion unit and the uncore and reaches the core only
through a registered `full`.

### Store-traffic strategy — store buffer first, write-back L2 only if it earns it

Store performance has two distinct costs:

- **Write latency** — how long the pipe waits on each store. In an
  in-order machine a store produces no register result, so only a
  blocking store path or a full buffer makes the core wait.
- **Write throughput** — the sustained rate the memory side absorbs
  stores. It binds only when stores arrive faster than they drain.

Measured on gen2.5 hardware, the binding cost is latency. Dhrystone
spends 26 % of its cycles waiting on stores (5 cycles each), while
draining the same stores in the background would keep the write path
busy about a quarter of the time. Below the arbiter a store takes 3
cycles whether it hits L2 or not: the SDRAM adapter releases a write as
soon as its CDC accepts it, so the SDRAM leg is already posted. The
measurements and the analysis are in
[`memory-performance.md`](./memory-performance.md).

That sets the order:

- **Phase-1 store-completion model: blocking write-through, WnA.** A store
  writes through L1-D to L2 and the core waits for the ack. Correct and
  simple; slow-but-correct, and because the cost is a *stall* (CPI), not a
  combinational path (fmax), it is safe to leave until the store buffer.
  A later load misses WnA L1-D and fills from L2, which already holds the
  completed store.
- **A store buffer is the primary store-traffic fix.** Cacheable stores
  retire into a small FIFO between the completion unit and the uncore
  and drain in the background. It needs no load disambiguation: L1-D is
  updated in place at commit on a hit, and every access that goes to L2
  (a fill, an uncached load) and every uncached store waits for the
  buffer to drain. It meets the core only through registered signals
  (`full` into the freeze), so it lives outside the core timing cones and
  is *safe to defer* (see the [deferral principle](#build-plan)). Its
  entries are lines with a byte mask: stores to one line merge, a
  kernel frame save of about 21 words fits in eight entries where it
  would overflow eight words, and an entry drains as one bus burst
  that the SDRAM adapter can write as a single line
  ([store streams](./memory-performance.md#store-streams),
  [write combining](./memory-performance.md#sdram-side-write-combining)).
- **Write-back L2 is a measure-driven option, not a plan item.** A
  write-back hit completes at the L2 tag verdict, about as late as the
  posted write-through it replaces, so it does little for store latency.
  It reduces SDRAM write traffic, which matters only if stores prove
  throughput-bound, and it adds dirty/eviction logic in the L2 miss path
  plus kernel flush/invalidate work around other hardware's memory
  accesses.
- **L1-D write-allocate stays off the table for now.** Its allocation logic
  is complex and sits in the most critical timing cones, so it is a real
  fmax risk; investigate only once the basic design closes. Keep WT-WnA.

Whatever store path lands, the ordering contract is unchanged and rides
existing ops: an uncached store, to a device or any other memory with side
effects, is ordered after every earlier store (the store buffer drains ahead
of it), so no new ISA barrier is introduced. Sysreg writes stay outside that
ordering, so a sysreg-driven copy engine would need an explicit drain.

## Performance targets

Targets, not measurements — actuals get recorded in `benchmark/` once gen3
runs on hardware.

| Metric | gen3 target | Reference |
|---|---|---|
| Fmax | ULX3S: reliable 37.5 MHz across a seed sweep, with headroom for the staged features; Artix-7: whatever the full top closes at | gen2 topped at barely-37.5 |
| CPI, cache-hot tight loop | ~1.1–1.3 (forwarding from day one) | gen2 ~2.5–3.0 unforwarded |
| CMP+Bcc heavy | ~1.0–1.1 (flags forwarded, BTB) | — |
| L1 size | per board: ECP5 8 KB 2-way (gen2's hit logic); Artix-7 16 KB, with 4 ways on the fetch side if MEM2 cannot take them ([geometry data](./memory-performance.md#l1-geometry)) | gen2 4 KB (then 2-way) |
| Main TLB | 256×2 in BRAM ([refill data](./memory-performance.md#tlb-sets-and-a-cheaper-refill)) | gen2 32×2 |
| L2-hit fill | ~5 cycles ([line-wide transfer](./memory-performance.md#the-line-wide-transfer)) | gen2.5 16 |
| L2-miss fill | ~20–25 cycles ([design target](./memory-performance.md#design-target-memory-latency-in-cpu-cycles)) | gen2.5 38 with the line stack |
| Load-use stall | 1 (forwarded) | gen2 4 |
| Taken-branch bubbles | 0 predicted / full flush cold | gen2 3 |

## Build plan

The ordering is the point. Each phase's structural prerequisite was proven
in Phase 0; nothing is built on an unproven cone. fmax is always read on
the **full top**, never on a bare-core probe (a bare cone over-reports — the
project has measured ~55 vs ~31 MHz between the two).

**The deferral principle.** The anti-dead-end rule applies to *timing cones*
— combinational boundaries that set fmax. Those are proven in Phase 0 and
never deferred. It does **not** apply to *cycle-count* perf that lives
outside the core cones (store throughput, L2 policy): that is always safe to
defer, because improving it later never forces a core rewrite. gen2's fatal
error was conflating the two — it deferred a *structural boundary* as if it
were a perf knob. gen3 keeps them separate: defer freely outside the cones,
never inside them.

### Phase 0 — structural feasibility probes (the gate)

Prove the four load-bearing claims close with margin on a bare cone, each
wired with realistic fanout on both sides of the boundary (not bare cells).
Full per-probe specs — RTL to build, exact path to measure, pass threshold,
decision-on-fail — are in [`phase0-probes.md`](./phase0-probes.md).

The bars are 50 MHz ones: a non-BRAM cone must also close at ~65 MHz.
That is what 37.5 MHz on the full top asks of a bare cone, at the
probe-to-full ratio gen2 measured (~55 vs ~31 MHz); the Phase-1
full-top read calibrates the ratio for gen3.

| Probe | What it proves |
|---|---|
| P0.1 registered load-completion | `MEM2 hit/miss → registered load_pending → issue/scoreboard gate` is short; the gen2 busy→issue and busy→MMU floor cones are gone |
| P0.2 registered decode→issue | `pre-decoded FIFO slot → regmap → scoreboard/forward compare → issue` starts from flops and fits |
| P0.3 split MEM + BRAM TLB | MEM1-launch / MEM2-verdict absorbs the 5.8 ns BRAM Tco + tag/permission compare in the period |
| P0.4 CPU↔bus burst + SDRAM survival | registered line-transaction into the **real `sdram_bus_adapter`** streams without the gapped-read deadlock |
| P0.5 physical-tag compare *(proposed, added after exit)* | the L1's tag compare at the board's associativity, chained behind the BRAM TLB verdict, still closes in MEM2 and IF2 |

**Gate rule (the anti-dead-end rule).** If a probe is structurally short of
its bar, decide *at that probe* — redesign the boundary, add a stage, or
take the structure down (fewer ways, a smaller array). Never "wire it up
and fix later." A path that only closes on 1-of-N seeds is not a close.

### Phase 1 — correctness skeleton (boots conformance)

Full 7-stage pipeline using the proven boundaries: registered
load-completion, decode-on-enqueue, full operand + flag forwarding,
single-outstanding hold-buffer loads, static-not-taken branches resolved in
EX (no BTB yet), and the memory hierarchy in its
[shapes](#feature-set-whats-foundational-vs-staged): write-through L1 +
**write-through L2** in one uncore pipeline with the line-wide transfer
and fill-through, the 256×2 TLB, and the registered bus. **Store path:
blocking write-through, WnA** — no buffer, no disambiguation;
slow-but-correct, and a stall (CPI) outside the core cones, so safe to
leave for the Phase-4 store buffer. divmul reused from `common/`.
**Exit:** `isa/` conformance passes on the gen3 RTL runner.

### Phase 2 — BTB

Fetch-time direct-branch BTB (0-bubble correct taken-prediction). Return
and direction prediction are staged, each chosen by a predictor model
first. Re-run conformance + gen3 microarch regressions; re-read full-top
fmax.

### Phase 3 — NetBSD boot

Boot the kernel (same binary, CPUID-discriminated). Core is feature-complete
(forwarding + BTB) and correctness-validated against a real OS before perf
work begins.

### Phase 4 — store buffer (store-traffic perf)

The primary store-traffic optimization: a cacheable-only store buffer
between the completion unit and the uncore
([design](./memory-performance.md#store-buffer-at-the-core-boundary)),
its entries a line wide with a byte mask. It meets the core only through
registered signals, so it lands here, late, with no risk to the core
fmax proven in Phase 0. Write-back / write-allocate L2 follows *only* if
profiling after this shows stores throughput-bound. L1-D write-allocate
is investigated here at the earliest, and only if it can be shown not to
lengthen the critical cone.

### Phase 5 — floorplan + clock

The **one-time** floorplan pass — once, after all features land, never
per-feature. It turns structural headroom into a 37.5 MHz close across
the seed sweep, not into a higher clock.

## Reading guide

1. **This document** — overview + build plan (you are here).
2. [`timing-outline.md`](./timing-outline.md) — the structural
   rules (Rule Zero, per-subsystem design points) this plan rests on.
3. Per-area gen3 specs written so far:
   - [`mem-stage.md`](./mem-stage.md) — the MEM1/MEM2 memory stage and the
     duplicated BRAM TLB.
   - [`memory-completion.md`](./memory-completion.md) — the registered
     back-end stall and the load/store completion + fault-precision contract.
   - [`memory-performance.md`](./memory-performance.md) — measured
     memory-path costs, gen2.5 workload profiles, and the uncore levers
     they point to (store buffer, one uncore pipeline, whole-line SDRAM
     fetch, stream prefetch, next-line prediction for L1-I).
4. Further per-area gen3 specs — *to be written as each subsystem is
   designed* (design-decisions, pipeline-stages, hazard-model,
   memory-interface), following the gen2 doc layout under `penumbra2/`.

For the system the gen3 core attaches to but does not change — ISA, ABI,
exception model, bus protocol, autoconfig, peripherals — the
[documentation index](../../README.md) (system reference + shared
internals) remains the source of truth.

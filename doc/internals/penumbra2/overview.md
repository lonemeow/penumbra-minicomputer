# Penumbra/2 — Overview

> **Applies to:** Penumbra/2 · pipelined core.

**Penumbra/2** is the second-generation Penumbra CPU. It implements
the same ISA as Penumbra/1, runs the same NetBSD kernel and
userland, and shares almost all of the surrounding system (bus,
peripherals, MMU, L2 cache, SDRAM controller) — but the CPU core
itself is **a hardwired, pipelined, FPGA-native design** rather
than Penumbra/1's single-cycle, microcoded, discrete-feasible
core.

This document is the entry point for anyone reading about gen2 for
the first time. It explains *why this design exists* (both
educationally and pragmatically), the broad architectural shape,
and the gen2 / gen2.5 / future roadmap, then points at the detailed
specs.

## Project context: learning CPU design evolution

Penumbra is fundamentally a **learning project about how CPU
architecture evolved over time, built by working through each
major historical phase one CPU at a time**. Each Penumbra
generation maps to a different era of the field; the *transitions*
between them are the most educational part, because they force
confrontation with the questions each new era's designers had to
answer.

| Penumbra | Era it represents | Defining features |
|----------|-------------------|-------------------|
| /1 | Classic discrete-logic minicomputer / early-microprocessor era (~1970s through early 1980s) | Microcoded control driving a single-cycle datapath, designed to be feasible in discrete 74xx TTL |
| **/2 gen2** (this design) | **Simple pipelined RISC era (late 1980s)** — MIPS R2000/R3000, early SPARC, Berkeley/Stanford RISC | **Classic in-order pipeline, hardwired control, BRAM-backed caches, flag-only forwarding, no prediction** |
| /2.5 (next planned phase) | Early-1990s pipelined-RISC polish — MIPS R3000 → R4000, SPARC v8 → v9 | Result forwarding, regfile write-through, branch prediction |
| /3 (speculative future) | Mid-to-late-1990s superscalar / out-of-order — Pentium Pro, MIPS R10000, Alpha 21164/21264 | Register renaming, multi-issue, reorder buffer, speculative execution |

Penumbra/2 is the project's next major **era transition**, not just
an incremental performance upgrade. Building it forces confrontation
with the questions the late-1980s RISC designers answered:

- **Why did pipelining displace microcode as the dominant control
  strategy?** In a single-cycle design, microcode *is* the
  sequencer — it dispatches a sequence of micro-ops to a datapath
  that has nothing else driving it. In a pipeline, the pipeline
  itself sequences work across stages, and microcode's role
  disappears for most instructions. (Penumbra/2 has zero
  microcode; see [Decision 6](./design-decisions.md#6-control-architecture-pure-hardwired-no-microcode).)
- **Why did caches need to grow large enough to require BRAM?**
  Because a pipelined CPU consumes instructions at a much higher
  rate than a single-cycle one, so memory bandwidth becomes the
  binding constraint. Distributed-RAM caches that worked fine for
  gen1 don't scale; BRAM with multi-cycle access does. (See
  [Decision 11](./design-decisions.md#11-bram-backed-caches-with-single-mem-stall).)
- **Why are hazards the central problem of pipelined designs?**
  Because parallelism makes ordering matter. In a single-cycle
  design every instruction completes before the next begins, so
  RAW dependencies are trivially satisfied. Once instructions
  overlap, hardware must either stall or forward — and the
  mechanisms for doing this precisely (scoreboarding,
  drain-commit, back-pressure, fault propagation) become the bulk
  of the design's complexity. (See
  [Decision 4](./design-decisions.md#4-hazard-handling-strategy),
  [Decision 9](./design-decisions.md#9-drain-commit-primitive),
  [Decision 10](./design-decisions.md#10-stall-propagation-policy-back-pressure).)

The pragmatic performance benefits of gen2 (higher fmax, larger
caches, better wall-clock performance once gen2.5 lands) are real,
but they are a **side effect** of the era transition rather than
the primary motivation. If we just wanted faster Penumbra/1, we'd
keep optimizing it; we don't, because the *learning* lives in the
transition itself.

The same logic governs the gen2 / gen2.5 split: gen2 deliberately
ships *without* general operand forwarding or prediction so that the
pipelined-but-unforwarded datapath is studyable in its own right,
before the gen2.5 optimizations obscure it. The one deliberate
exception is **NZCV flag forwarding** — pure-stall on flags would make
every conditional branch pay ~3 cycles and dominate CPI, and the fix is
both cheap and *simplifying* (it removes NZCV from the scoreboard), so
gen2 takes it
([Decision 12](./design-decisions.md#12-nzcv-flag-forwarding)). GPR/SPR
forwarding stays in gen2.5. Building gen2.5 on top of gen2 will then
teach what each remaining optimization individually buys.

## Why a second generation?

Penumbra/1 has reached the natural ceiling of its single-cycle
microcoded architecture:

- **Fmax** is capped by the full single-cycle datapath
  (µROM → decode → regfile → ALU → flag capture) forming one
  combinational cone: RTL work can move the limiter around inside
  the cone but cannot shorten the cone itself. Further gains
  require *splitting the cycle* — i.e., pipelining.
- **L1 cache** is capped at ~1 KB because the distributed-RAM
  storage that lets the gen1 cache hit combinationally doesn't
  scale past that without LUT explosion. Larger caches need BRAM,
  which has registered output and forces multi-cycle access.
- **CPI** is locked at 1.0 by definition (single-cycle), but the
  achievable clock is so low that the wall-clock performance is
  modest. A pipelined design at ~2 CPI needs ~2× the clock just to
  break even — the real payoff is the headroom it opens: forwarding
  and prediction then convert the clock surplus into wall-clock
  wins the single-cycle design structurally cannot reach.

Penumbra/2 takes the natural next step: pipeline the design, use
BRAM for caches, drop the discrete-74xx feasibility constraint
that drove a lot of gen1's choices, and aim for fmax and cache
sizes that match real-world FPGA RISC softcores (VexRiscv, Ibex,
Rocket).

Gen1 is **not deprecated** — it remains the canonical
discrete-feasible design and continues to demonstrate the
classical single-cycle/microcoded path. Penumbra/2 evolves
alongside it in a separate `hw/rtl/penumbra2/` tree, sharing
peripherals and infrastructure.

## What gets shared, what gets forked

The split rule is **shared = bus-facing or storage; forked =
pipeline-shaped**.

| Component | Disposition | Where |
|-----------|-------------|-------|
| ISA, ABI, exception model | **Shared spec** | `doc/system/` |
| Peripheral RTL (UART, SPI, SDRAM, autoconfig, timer) | **Shared, unchanged** | `hw/rtl/io/`, `hw/rtl/soc/` |
| MMU and TLB modules | **Shared, unchanged** | `hw/rtl/mmu/` |
| L2 cache | **Shared, unchanged** | `hw/rtl/soc/l2_cache.sv` |
| L1 cache | **Forked** — new BRAM-backed module | `hw/rtl/penumbra2/cache_bram_vipt.sv` (new) |
| CPU core | **Hard fork** — pipelined, hardwired | `hw/rtl/penumbra2/` (new) |
| FPGA top-levels | **Side-by-side** | `hw/rtl/fpga/ulx3s/ulx3s_penumbra{1,2}_top.sv` |
| Microcode (gen1) | **gen1-only**, never used by gen2 | `hw/microcode/`, `hw/tools/uasm.py` |
| NetBSD kernel | **Shared binary** — CPUID-discriminated at boot | `netbsd/sys/arch/penumbra/` |

The naming convention `penumbra1` / `penumbra2` (not "core1/core2",
not "gen1/gen2") matches the CPU's self-identification — see the
project memory note for the convention's rationale.

## Architecture in broad strokes

**Pipeline shape: 6 stages**, IF1 / IF2 / ID / EX / MEM / WB. The
split IF is forced by the BRAM-backed I-cache (1-cycle BRAM access
latency must be a pipeline stage, since it's paid every fetch).
MEM stays a single stage and asserts a 1-cycle STALL only when a
load or store actually accesses D-cache, so ALU/branch/sysreg-
internal instructions don't pay the BRAM cost.

```
IF1: drive cache addr to BRAM, TLB lookup combinational
IF2: BRAM output, tag compare, fault detection
ID:  decode, regfile read, scoreboard check
EX:  ALU, flag compute, branch resolve, drain-commit, divmul
MEM: D-cache access (STALL 1 cycle on hit), MMU, sub-word
WB:  regfile write (2 ports), SR/SPR write, scoreboard clear
```

**Control: pure hardwired, no microcode.** Each stage has its own
combinational decoder. The only stateful sequencer in the entire
core is a ~3-state vector-fetch FSM in IF1 for exception entry.
Gen1's microcode infrastructure does not migrate.

**Hazards: pure stall for GPR/SPR, forwarding for flags (gen2).** A
unified physical-addressed scoreboard with ~21 valid bits (R1–R13, USP,
SSP, ESR, EPC, SCR0–3) gates issue at ID; a dependent instruction stalls
~3 cycles waiting for the producer to commit at WB. The physical
addressing handles the R14 ↔ USP/SSP banking aliasing correctly. NZCV is
*not* in the scoreboard — it is forwarded MEM/WB→EX, so flag readers
(Bcc, ADC/SBC) never stall
([Decision 12](./design-decisions.md#12-nzcv-flag-forwarding)).

**Branches: static-not-taken, resolved in EX.** 3-bubble flush
penalty on taken branches.

**Two architectural special mechanisms:**

- **Drain-commit primitive.** ERET and WRSYS hold in EX until
  MEM/WB drain, then commit directly from EX. This handles
  precise-exception ordering w.r.t. older faults AND sysreg-side-
  effect ordering w.r.t. younger instructions (e.g., `WRSYS TLB;
  ERET` to user mode). WRSYS additionally waits one cycle
  post-commit for the sysreg device to latch.
- **2R/1W regfile with sequenced divmul writeback.** The Penumbra
  ISA's MUL/DIV write two GPRs per instruction (`Rd` low half +
  `Rdh` high half). Rather than a true second write port (which
  ECP5 1W/1R distributed RAM can't provide by replication), divmul
  sequences its two writes through the single port over two
  consecutive cycles, holding the pipeline one extra cycle — cheap,
  since it already stalls ~33 cycles. The two read ports come from
  replicating the distributed RAM. See
  [regfile.md](./regfile.md).

**Caches: BRAM-backed VIPT, scalable to 4 KB+.** Tags + data +
valid arrays in BRAM with registered address (REGMODE_A=NOREG so
output is combinational during the next cycle). Same VIPT
precondition as gen1 (cache ≤ page size).

**Stall propagation: back-pressure.** Stalled stage holds; upstream
back-pressure cascade holds; downstream drains naturally.

## Performance targets

| Metric | gen2 target | gen2 realistic | gen2.5 target | Reference |
|--------|-----------|---------------|---------------|-----------|
| Fmax | ≥ gen1's clock | ~2× gen1 (cache no longer in critical path) | ≥2× gen1 | gen1's ceiling is the single-cycle cone |
| CPI on tight loops, cache hit | ~2.0 | ~2.5-3.0 (no forwarding) | ~1.2-1.5 | gen1 is 1.0 at lower clock |
| CPI on CMP+Bcc heavy code | — | ~1.2 (flags forwarded; taken-branch flush remains) | ~1.0-1.1 (+prediction) | — |
| I-cache size | 4 KB | 4 KB | 8-16 KB | gen1 is 1 KB |
| Branch flush bubbles | 3 | 3 | 0-1 (with prediction) | — |
| Load-use stall | 4 | 4 (no forwarding) | 1 (with forwarding) | — |

Wall-clock performance comparison vs gen1 depends entirely on the
fmax achieved. At ~2× gen1's clock with ~2.5 CPI, gen2 lands
roughly even with gen1. The win comes in gen2.5 once forwarding
lands and CPI drops toward 1.2.

## gen2 / gen2.5 / future roadmap

**gen2 — correctness first.** Ship a working 6-stage pipelined
Penumbra/2 that boots the same NetBSD kernel as Penumbra/1. No GPR/SPR
forwarding, no prediction, no fancy optimizations — only the targeted
NZCV flag bypass. The point is to validate that the architecture is
correct, that the regfile and scoreboard work, that drain-commit
handles ERET and WRSYS correctly, that exception entry is precise, that
MUL/DIV's two-write commit is reliable. **CPI on GPR-dependent code
will be unimpressive (~3) but the architecture will be sound.**

**gen2.5 — forwarding, prediction, and store buffering.** With gen2
proven correct, add the textbook performance features:

- **EX→EX forwarding** for ALU results (eliminates most RAW
  stalls).
- **MEM→EX forwarding** for loaded values (eliminates most
  load-use stalls; 1-cycle load-use distance remains).
- **WB→ID regfile write-through** (eliminates the last RAW
  stall cycle).
- **SPR scoreboard forwarding** (eliminates RAW stalls between
  WRSPR and immediately-following RDSPR).
- **Static or bimodal branch prediction** in IF1 (reduces
  taken-branch flush from 3 bubbles to 0 on correctly-predicted
  branches, ~80-90% of taken branches in typical code).
- **A cacheable-only store buffer** that decouples a store's commit
  from its write-through to L2, so a store no longer stalls the
  pipeline for the L2 write latency. Uncacheable stores drain the
  buffer and issue synchronously, which keeps MMIO writes in program
  order, makes every prior cacheable store visible before a DMA
  kickoff, and keeps load disambiguation simple (only cacheable loads
  consult the buffer; an MMIO read is a different cacheability and
  never aliases a buffered entry). It sits on the existing
  write-through path — distinct from, and lighter than, the gen3
  write-back L2 protocol reshape.

These are well-trodden mechanisms — the gen2 pipeline-register
layout is already forward-compatible with adding them, and each
gen2.5 addition is a new leaf module plus the integration wiring
that connects it. How this maps onto the source tree — gen2's cells
shared unchanged, only the integration chain forked — is covered in
[gen2.5 organization](#gen25-organization-composition) below.

**gen3 (speculative — not yet planned).** Possibilities include:

- Larger / more associative L1 caches.
- Split MEM (MEM1/MEM2) if memcpy/memset throughput becomes the
  binding bottleneck.
- L2 cache protocol upgrade to line-granular transfers (already
  noted as gen2's natural follow-on).
- Superscalar issue (probably not — single-issue keeps the
  design within the project's scope).
- Out-of-order completion (probably never — too complex for the
  intended scale).
- Hardware FPU (currently software-emulated, same as gen1).

## gen2.5 organization: composition

gen2.5 is built by **composition**, not by parameterizing gen2. gen2
must remain readable as the machine it is — a pipelined core with no
notion of forwarding, prediction, or a store buffer. A design that
carried those mechanisms with their controls strapped to a default
would no longer *be* gen2; it would be gen2.5 with the optimizations
switched off, a different artifact. For a project whose deliverable is
the sequence of generations themselves, that distinction is the point.

So the split is by role. **Leaf cells are shared, unchanged**: the
ALU, regfile, scoreboard, flag bypass, decoder, register map, the
per-stage datapath cells, and the entire MMU / TLB / cache / arbiter
stack have no generation-specific behavior, so gen2.5 instantiates the
exact same modules. They hold the bulk of the correctness-critical
logic and stay single-source — a fix lands once and serves both
generations. **The integration chain is forked**: the files that wire
those cells into a pipeline — machine, core, spine, and the ID/EX
stage assemblies — are where a microarchitecture's identity lives and
where gen2.5 differs (a predictor wired in, a prediction tag riding the
ID/EX register, EX resolving on misprediction). gen2.5 gets its own
copies; gen2's stay byte-frozen. New gen2.5-only behavior lands as new
leaf modules — a predictor, a forwarding network, a store buffer —
that the forked integration files instantiate.

This still gives the honest comparison that motivated sharing: gen2 and
gen2.5 build from the *same live* leaf library and the *same current*
compiler, so a CPI or DMIPS delta isolates the optimization — without
freezing gen2 in time (a tagged snapshot would be measured against
whatever the backend looked like at tag time) and without contaminating
gen2's structure (a parameterized tree would). The cost, stated
honestly: the forked integration files are mostly shared-but-not-leaf
logic — the drain-commit FSM, fault flush, the back-pressure
handshakes — so an integration-level fix applies to both copies.
Composition *bounds* that dual-fix surface to the integration chain
(the leaf cells never duplicate) but does not erase it there; the
discipline that keeps it tractable is to hold each forked file a
*minimal diff* against its gen2 original, so porting a shared fix is a
small, reviewable change.

**RTL.** The gen2.5 behavior lives in new leaf modules — a forwarding
network, a branch predictor, a store buffer — plus the forked
integration files that wire them in (operand-source muxes, the relaxed
hazard conditions a forward now permits, the misprediction redirect).
Because gen2's own stage files are untouched, "the unforwarded datapath
is studyable in its own right" is not a gated leg that has to be
asserted clean — it is simply what gen2's files *are*. That is the
reason for composition over an in-place parameter: gen2's structure
states the absence of these mechanisms by construction.

**Build.** A `CORE=penumbra<n>_<sub>` selector (for example
`penumbra2_5`) selects the gen2.5 fileset: the shared leaf cells plus
the forked integration chain, in place of gen2's integration files. The
runner configuration and the program suite come from the base
generation, so a sub-variant cannot drift from the base's test setup;
the genuinely per-variant facts are which integration files it forks
and the capability delta below.

**Test.** A variant inherits the base's entire program suite. Almost
all of it passes unchanged, because the optimizations preserve
architectural semantics and change only timing. The exceptions are
tests that assert an exact cycle or stall count, which forwarding
legitimately alters; these carry a `pinned-stalls` capability tag that
the baseline provides and the optimized variant does not, so the runner
skips them visibly rather than reporting a false failure. Identifying
them is not upfront work — it is the failure set from running the
inherited suite against the optimized build during bring-up: a failure
on a *result* is a forwarding bug to fix, a failure on a *cycle count*
is a timing-pinned test to tag. Inheriting everything is what makes a
mis-wired forward surface as an immediate, localized failure in a test
that would otherwise never have exercised the new variant. Tag
mechanics live in the [test-suite conventions](../build-system.md).

## Reading guide

If you're new to gen2, read in this order:

1. **This document** — overview (you are here).
2. **[design-decisions.md](./design-decisions.md)** — the
   architectural decisions with full rationale, alternatives
   considered, and consequences. Read in order; each one builds
   on earlier ones. Skim or skip the decisions whose conclusions
   you already accept from this overview.
3. **[pipeline-stages.md](./pipeline-stages.md)** — the pipeline
   specification proper. Per-stage description, inter-stage
   register layouts, stall/flush semantics, six cycle-accurate
   timing examples covering the most common pipeline behaviors.

The per-area specs go deeper:

- [`control-decode.md`](./control-decode.md) — per-format decoder,
  IR → control bundle.
- [`hazard-model.md`](./hazard-model.md) — scoreboard mechanism in
  detail.
- [`exception-flow.md`](./exception-flow.md) — fault propagation,
  vector-fetch FSM, IRQ drain-and-take, ERET cycle-by-cycle.
- [`regfile.md`](./regfile.md) — 2R/1W regfile (read-port
  replication, single-port divmul write sequencing), R14 banking,
  USP/SSP storage.
- [`memory-interface.md`](./memory-interface.md) — L1↔L2 arbitration,
  the line fill path, and the transaction taxonomy (uncacheable /
  sub-word / write handling).
- [`fpu.md`](./fpu.md) — forward-looking ISA design for a hardware
  FPU (GPR-resident floats, IEEE-754 correctness-first); floating
  point stays software-emulated until such a unit is built.

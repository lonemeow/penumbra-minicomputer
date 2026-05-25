# Penumbra/2 — Design Decisions

This document records the architectural and project-level decisions
made during the design of **Penumbra/2** (gen2) — the second-generation
Penumbra CPU. Each entry captures the *context* that prompted the
decision, the *decision itself*, the *rationale* over the alternatives
considered, and the *consequences* that follow.

This is a **gen2-scoped** design log. Decisions driving Penumbra/1 —
the single-cycle microcoded original — are out of scope; those are
implicit in the existing specs (`doc/system/`, `doc/internals/`,
`doc/internals/penumbra1/` once the rename lands) and were made under a
fundamentally different constraint set (discrete 74xx feasibility,
which Penumbra/2 deliberately abandons).

ISA-level decisions are also out of scope — Penumbra/2 implements the
same ISA as Penumbra/1, so the ISA-level reasoning lives in
`doc/system/architecture.md` and friends.

## Scope of "design decision"

A design decision belongs here when **all** of the following hold:

- It is specific to Penumbra/2 (not project-wide convention, not
  ISA-level, not shared infrastructure).
- It selects between two or more reasonable alternatives.
- The rationale is non-obvious from the resulting code or specs alone.

Project-wide conventions (naming schemes, diagram tooling, repo
layout) and ISA-level commitments belong in their own places — see the
[Project-wide conventions](#project-wide-conventions-referenced-here)
section at the bottom of this document for pointers.

---

## Index

| # | Decision | Date |
|---|----------|------|
| [1](#1-project-goals-and-non-goals) | Project goals and non-goals | 2026-05-24 |
| [2](#2-rtl-and-doc-organization-shared-vs-forked) | RTL and doc organization: shared vs forked | 2026-05-24 |
| [3](#3-pipeline-stage-count-and-shape) | Pipeline stage count and shape | 2026-05-24 |
| [4](#4-hazard-handling-strategy) | Hazard-handling strategy | 2026-05-24 |
| [5](#5-branch-resolution-policy) | Branch resolution policy | 2026-05-24 |
| [6](#6-control-architecture-pure-hardwired-no-microcode) | Control architecture: pure hardwired, no microcode | 2026-05-24 |
| [7](#7-cache-and-mmu-reuse-strategy) | Cache and MMU reuse strategy | 2026-05-24 |
| [8](#8-netbsd-kernel-compatibility-approach) | NetBSD kernel compatibility approach | 2026-05-24 |
| [9](#9-drain-commit-primitive) | Drain-commit primitive | 2026-05-24 |
| [10](#10-stall-propagation-policy-back-pressure) | Stall propagation policy (back-pressure) | 2026-05-24 |
| [11](#11-bram-backed-caches-with-single-mem-stall) | BRAM-backed caches with single-MEM-STALL | 2026-05-24 |

---

## 1. Project goals and non-goals

**Date:** 2026-05-24

**Context.** Penumbra/1 reached a stable operating point of ~25 MHz on
the ULX3S (ECP5-85F sg6) with NetBSD booting to userland. Its
single-cycle microcoded datapath has hit its architectural floor — the
critical path is the canonical single-cycle compute path (µROM read →
decode → regfile → ALU → flag capture), and further fmax gains would
require splitting the cycle. That split is the natural starting point
for a second-generation core.

**Decision.** Penumbra/2 targets:

- **Same ISA as Penumbra/1**, fully binary-compatible with userspace.
  Same kernel ideally, with at most minor MD-code differences if a
  TLB-or-similar deviation proves too costly to avoid.
- **Classic 5-stage RISC pipeline** as the baseline microarchitecture,
  with the option to evolve to a deeper pipeline if synthesis shows a
  binding stage that splitting would relieve.
- **~2 CPI on tight loops with high cache hit ratio**, with 1.5 CPI as
  the eventual goal once gen2.5 features land.
- **≥25 MHz fmax** on the same ULX3S target; higher is welcome but not
  required for gen2.
- **FPGA-native**: no discrete-74xx feasibility constraint. Use BRAMs,
  DSP blocks, distributed RAMs, multi-port memories — whatever ECP5
  offers — to hit fmax and CPI targets.
- **Reuse non-CPU RTL unchanged**: peripherals on the Penumbra bus
  (UART, SPI, SDRAM, autoconfig, timer, etc.) and the MMU/cache
  modules are shared with Penumbra/1.
- **Correctness first, optimization second**: gen2 ships without result
  forwarding or branch prediction. Those are gen2.5 scope.
- **Hardware MUL/DIV is gen2 scope**, not deferred. Penumbra/1 will
  receive a hardware divmul unit before Penumbra/2 work begins, and
  the NetBSD kernel uses MUL/DIV heavily — falling back to software
  emulation in gen2 would give up substantial real-workload
  performance. The divmul unit writes two GPRs per instruction
  (`Rd` low half + `Rdh` high half for MUL; `Rd` quotient + `Rdh`
  remainder for DIV; analogous for DIVL/DIVLU) — see
  [Decision 4](#4-hazard-handling-strategy) for the regfile
  implication.
- **New ISA exception: `VEC_ARITH`** added at vector slot 10 (currently
  reserved). Raised by the divmul on DIV-by-zero and DIVL overflow.
  This is the only ISA-visible addition gen2 makes beyond Penumbra/1's
  current spec; userland is unaffected because the trigger is a
  programmer error.

**Rationale.** The "FPGA-native, performance-first, correctness-before-
cleverness" framing is what justifies the entire effort: if Penumbra/2
weren't allowed to outgrow the discrete constraint, the fmax and CPI
goals would be unreachable; if it weren't required to keep the ISA,
the entire NetBSD toolchain and userland would need to be re-targeted.
The 2 CPI target is the threshold below which a 5-stage pipeline is
clearly better than the current single-cycle design (which is by
definition 1 CPI but at a much lower clock).

**Consequences.**

- Penumbra/1 is **not deprecated.** It remains a working,
  demonstrable, demo-able design and may continue to receive small
  fixes and enhancements. Penumbra/2 develops alongside it, not on top
  of it.
- Anything that needs a discrete-74xx-feasible expression (the
  eventual chip-level rebuild) targets Penumbra/1, not Penumbra/2.
- The shared bus protocol, sysreg sideband, and Penumbra bus contract
  are load-bearing for the reuse goal — they must not regress in
  Penumbra/1 work either.

**Alternatives considered.** A from-scratch new ISA (rejected: kills
the userland and the NetBSD port). A drop-in superscalar (rejected for
gen2: too large a leap, no learning value; viable as gen3 some day).
Continuing to optimize Penumbra/1 toward higher fmax (rejected: the
remaining levers all require splitting the cycle, which is exactly
what a pipeline is — and a pipeline is more interesting to design).

---

## 2. RTL and doc organization: shared vs forked

**Date:** 2026-05-24

**Context.** Penumbra/1 and Penumbra/2 must coexist in the repository
indefinitely. Both will need to build, run tests, and synthesize for
the ULX3S. Some modules (peripherals, caches, bus) are reusable; the
CPU core itself is a hard fork.

**Decision.** Fork only the CPU core; share everything else.

- `hw/rtl/penumbra1/` — current CPU core (renamed from `hw/rtl/core/`)
- `hw/rtl/penumbra2/` — new pipelined CPU core
- `hw/rtl/mmu/`, `hw/rtl/soc/`, `hw/rtl/io/`, `hw/rtl/bus/`, `hw/rtl/sim/`
  — **shared, unchanged**
- `hw/rtl/fpga/ulx3s_top_penumbra1.sv` and
  `hw/rtl/fpga/ulx3s_top_penumbra2.sv` — two top-levels, side by
  side, selectable by `make fpga TOP=…`
- `hw/rtl/sim/machine_sim_penumbra1.sv` and `_penumbra2.sv` — likewise
- `hw/microcode/` and `hw/tools/uasm.py` — **penumbra1-only**; not
  used by penumbra2

Docs follow the same shape:

- `doc/internals/penumbra1/` — gen1-only docs (`datapath.md`,
  `microcode.md`, `uasm-syntax.md`)
- `doc/internals/penumbra2/` — gen2 docs (this file,
  `pipeline-stages.md`, `control-decode.md`, `hazard-model.md`,
  `exception-flow.md`, `regfile.md`)
- `doc/internals/` (root) — shared (`l2-cache.md`,
  `sdram-controller.md`, `sdram-optimization.md`,
  `coding-standards.md`, `cpu-bus.md`, `mmu-internals.md`, `setup.md`)
- `doc/system/`, `doc/hardware/` — shared, unchanged (ISA, bus
  protocol, device specs)

**Rationale.** "Shared = bus-facing, forked = pipeline-shaped" is the
clean cut. Peripherals talk the Penumbra bus protocol, which is the
contract that both cores honor; they don't care which side of the bus
the CPU is. The CPU core, by contrast, is fundamentally restructured
between gen1 (microcoded single-cycle, three-bus datapath) and gen2
(hardwired pipelined, per-stage), so trying to parameterize the same
modules to cover both would create constant friction. A flat
`penumbra1`/`penumbra2` directory split was preferred over
`hw/rtl1/`/`hw/rtl2/` (which would duplicate peripherals) and over a
single `core/` with a `parameter GEN` (which would mix two very
different architectures in one file).

**Consequences.**

- A reorganization commit will rename `hw/rtl/core/` → `hw/rtl/penumbra1/`
  and move three docs (`datapath.md`, `microcode.md`,
  `uasm-syntax.md`) under `doc/internals/penumbra1/`. The rename
  touches imports in `machine_sim.sv` and `ulx3s_top.sv` and CLAUDE.md
  paths.
- Any future RTL coding standard, bus protocol clarification, or cache
  policy change must consider both cores. Penumbra/2 is **never**
  allowed to introduce changes to shared modules that break
  Penumbra/1.
- The build system gains parallel paths: `make fpga TOP=ulx3s_top_penumbra2`,
  `make sim MOD=cpu_penumbra2`, etc.

**Alternatives considered.** Fully parallel `hw/rtl1/`/`hw/rtl2/`
trees (rejected: forces peripheral duplication or symlinks; bug fixes
diverge). Per-module versioning with `cpu_core_penumbra1.sv` /
`cpu_core_penumbra2.sv` in the same directory (rejected: file
filenames carry both the role and the generation, making greps and
navigation harder). Single parameterized `core/` (rejected: too
divergent at the module level).

---

## 3. Pipeline stage count and shape

**Date:** 2026-05-24

**Context.** Classic 5-stage RISC (IF / ID / EX / MEM / WB) is the
textbook baseline, but Penumbra-specific constraints could justify a
different shape: the current IF-equivalent path (PC → TLB → I-cache
tag compare + data read) is tight at 25 MHz as one stage, and the
symmetric MEM stage has the same concern. Splitting either into two
stages would relieve that pressure at the cost of an extra branch- or
load-bubble.

**Decision.** Target **6-stage IF1 / IF2 / ID / EX / MEM / WB** for
gen2. The originally-considered 5-stage classic is rejected in favor
of split IF + single MEM-with-STALL, driven by the BRAM-backed cache
choice in [Decision 11](#11-bram-backed-caches-with-single-mem-stall).
MEM remains a single stage; loads/stores assert a 1-cycle STALL on
cache hit to absorb the BRAM-output latency without committing the
pipeline depth that a split MEM would cost.

| Stage | Work |
|-------|------|
| IF1   | PC reg, drive cache address to BRAM (combinational), TLB lookup (combinational from PC), vector-fetch FSM |
| IF2   | BRAM output appears (clocked from IF1's registered address), tag compare against TLB-provided paddr_tag, data mux, fault detection |
| ID    | Combinational decoder, regfile read, scoreboard check, stall logic |
| EX    | ALU compute, flag capture, branch target computation + condition, ERET / WRSYS drain-commit, divmul start/busy |
| MEM   | D-side BRAM cache access (1-cycle STALL on hit to absorb BRAM output latency; longer on miss), MMU integration, alignment check, sub-word LD extract / ST replicate, sysreg sideband for RDSYS |
| WB    | Regfile write (2 ports — main + divmul-high), SR/SPR write, scoreboard clear |

**Rationale.** Classic 5-stage was the original target, but the
BRAM-backed cache decision ([Decision 11](#11-bram-backed-caches-with-single-mem-stall))
makes single-cycle IF infeasible: BRAM with registered output has
1-cycle latency from address to data, requiring either a split IF
stage or a STALL on every fetch. Split IF is the standard
softcore answer (VexRiscv, Rocket, Ibex), so we adopt it.

MEM is treated asymmetrically: a single stage with optional 1-cycle
STALL on cache hit. This bills the BRAM-latency cost only to
instructions that access D-cache (~25-30% of insns), preserves the
3-bubble branch flush (instead of growing to 4), and reuses the
existing STALL infrastructure rather than building parallel
pipelining for the D-side. Per [Decision 11](#11-bram-backed-caches-with-single-mem-stall)
this is the better trade for Penumbra's workloads, where I-fetch
latency dominates and memcpy-class workloads are SDRAM-bandwidth-
bound regardless.

**Consequences.**

- Branch flush penalty is **3 bubbles** (IF1, IF2, ID after a taken
  branch — see [Decision 5](#5-branch-resolution-policy)).
- Load-use distance is **4 cycles** (load completes WB 1 cycle later
  than ALU due to MEM-STALL; dependent ALU op stalls 4 cycles in ID
  under pure-stall, see [Decision 4](#4-hazard-handling-strategy)).
- The TLB stays as distributed-RAM async lookup (gen1-inherited);
  its output is registered at end of IF1 alongside the BRAM cache
  address, then used in IF2's tag compare.
- ALU-only insns pay no MEM penalty — they pass through MEM in 1
  cycle. Load/store insns pay 1 cycle of stall per access (BRAM
  output latency).
- The pipeline-stage layout is forward-compatible: gen2.5+ forwarding
  reduces the load-use stall to 1 cycle; gen3 may revisit split-MEM
  if memcpy/memset throughput becomes the dominant bottleneck.

**Alternatives considered.** 5-stage with distributed-RAM cache
(rejected per [Decision 11](#11-bram-backed-caches-with-single-mem-stall):
caps cache size at ~1 KB, blocks fmax beyond ~30 MHz). 7-stage with
split MEM (rejected: adds branch-flush bubble and exception-drain
cost without proportional benefit for Penumbra's typical workloads).
Decide-after-synth-experiment (rejected: spec drift while waiting on
measurement).

---

## 4. Hazard-handling strategy

**Date:** 2026-05-24

**Context.** A pipelined design must handle data hazards (RAW
dependencies between in-flight instructions). The textbook spectrum
runs from pure stall (no forwarding, scoreboard) through cheap regfile
write-through to full EX→EX + MEM→EX forwarding networks. Each step
adds wiring, mux logic, and verification surface in exchange for CPI.

**Decision.** **Pure stall, no forwarding, no regfile write-through**
in gen2. ID stalls on any RAW conflict until the producer fully retires
through WB. Forwarding lands in gen2.5.

**Mechanism.** A **unified physical-addressed scoreboard** with ~22
valid bits, one per scoreboardable physical storage element:

| Physical entry | Source(s) of ISA-level access |
|----------------|--------------------------------|
| R1–R13 (13) | Normal ALU / load / store / branch register references |
| **USP** | `R14` in user mode; `RDSPR/WRSPR USP` (regardless of mode — cross-bank access in supervisor mode) |
| **SSP** | `R14` in supervisor mode |
| ESR | `RDSPR/WRSPR ESR`; hardware exception entry (direct flop write, bypasses scoreboard) |
| EPC | `RDSPR/WRSPR EPC`; hardware exception entry (direct flop write, bypasses scoreboard) |
| SR | `RDSPR/WRSPR SR`; `EI`/`DI`; flag-writing ALU ops (write side); Bcc (read side) |
| SCR0–SCR3 (4) | `RDSPR/WRSPR SCRn` only |

R0 and R15 are excluded (R0 reads always zero; R15 reads come from
the PC register, never the regfile; neither is ever a write
destination from instructions that need RAW tracking).

**Issue and clear.** On instruction issue at ID, the destination
physical entry's valid bit is cleared. On WB completion (or divmul
completion for the high half), the bit is set. ID stalls if any
source physical entry's valid bit is clear. ID-stalled cycles
preserve the IF/ID register contents; ID/EX gets a NOP bubble per
stall cycle. Hardware writes to ESR/EPC during exception entry
bypass the scoreboard (they're direct flop writes during the
save-state pulse, not pipelined).

**Decoder's ISA→physical mapping.** The ID-stage decoder produces
`phys_src_a`, `phys_src_b`, `phys_dst` from the instruction opcode +
the current `SR.S` mode bit. The two non-obvious mappings are:

- `R14` → USP if `SR.S = 0`, SSP if `SR.S = 1`
- `RDSPR/WRSPR USP` → USP regardless of mode (cross-bank access from supervisor mode is handled by the regfile when the decoder asserts `cross_bank` for SPR=USP)

All other mappings are straightforward (architectural name → physical
entry of the same name).

**Why physical-addressed.** Architectural R14 aliases USP in user
mode and SSP in supervisor mode. A scoreboard indexed by architectural
name would miss the RAW hazard between a supervisor-mode `WRSPR USP, R1`
and a subsequent user-mode `R14` read after ERET — they touch the
same physical register but architectural-name scoreboarding sees them
as different (one targets "USP", the other targets "R14"). Physical
addressing makes the aliasing visible to the scoreboard.

**Quiescence of `SR.S`.** The decoder's R14 mapping is correct only
if no in-flight instruction is in the process of changing `SR.S`
between an instruction's decode and its commit. This is guaranteed
because every mode-changing operation either drain-commits (ERET,
WRSPR SR — both per [Decision 9](#9-drain-commit-primitive)) or
fires after pipeline drain (exception entry save-state pulse,
IRQ drain-and-take). So when an instruction is decoded in ID, the
`SR.S` value it sees is stable for the duration of that instruction's
pipeline residence.

**Rationale.** Pure stall is the simplest correct hazard-handling
mechanism. With no forwarding muxes there is nothing to mis-wire; with
no write-through there is nothing subtle about when a written value
becomes readable. This makes gen2 verifiable as a self-contained design
before the more interesting (and more bug-prone) forwarding work
begins. The CPI cost is real — a dependent pair stalls 3 cycles
instead of 0 — but the architecture leaves room for forwarding to
reclaim it in gen2.5 without redesigning anything in gen2.

**Consequences.**

- Realistic gen2 CPI is ~3 on dependent code (e.g., Dhrystone-shaped
  loops), ~1 on independent code (e.g., copy loops without
  inter-iteration deps). The 2 CPI target is reachable on suitably-
  structured code in gen2; the 1.5 CPI target requires gen2.5.
- The scoreboard is ~22 bits of state (one valid bit per physical
  entry; see Mechanism above). This includes SPRs (ESR, EPC, USP,
  SR, SCR0–3) which need hazard tracking like GPRs — the TLB miss
  handler in particular leans on RDSPR/WRSPR to the scratch SPRs as
  fast-access spill storage (`netbsd/sys/arch/penumbra/penumbra/locore.S`
  `_real_miss_handler`), and draining each WRSPR would tank that
  hot path.
- **Flag-write hazard (CMP → Bcc).** Most ALU ops write SR's flag
  bits. Bcc reads them. Under strict pure-stall, a dependent Bcc
  stalls ~3 cycles in ID waiting for the prior flag-writer to commit.
  This is the dominant CPI loss on branch-heavy code in gen2. Flag
  forwarding (a narrow EX→EX path, 5 bits) is the obvious fix and
  reclaims this; explicitly deferred to gen2.5 to preserve the
  no-forwarding correctness baseline. (The user pick was to take
  the CPI hit in exchange for cleaner gen2 verification.)
- The regfile is **2R/2W** in gen2 — two read ports for ID-stage
  operand reads (unchanged), two write ports because the divmul unit
  produces two-register results (see
  [Decision 1](#1-project-goals-and-non-goals)). Write port 1 is the
  main WB-stage writeback (used by every instruction that writes a
  GPR); write port 2 is dedicated to the divmul's `o_result_hi`
  output and is only used at MUL/DIV completion. On ECP5, 2W is
  implemented by **replicating** the distributed-RAM regfile: two
  physical 16×32 banks, both written on any write, reads from either
  bank. ~32 distributed-RAM LUTs of storage cost. Adding WB→ID
  write-through in gen2.5 is still a small edit (one mux on the read
  path).

**Alternatives considered.** Scoreboard + WB→ID write-through
(rejected for gen2: couples regfile timing to control, even though
"free"; trivial to add later). Full EX→EX + MEM→EX forwarding from day
one (rejected: more wiring and verification up front, and the
correctness work for gen2 is hard enough without the bypass network).

---

## 5. Branch resolution policy

**Date:** 2026-05-24

**Context.** Branches in a 5-stage pipeline must resolve somewhere.
The earliest possible resolution is ID (PC-relative target +
condition known from a flag-forwarding path); the standard MIPS choice
is EX (target and condition from EX outputs). Earlier resolution
reduces flush penalty but costs ID-stage logic; later resolution is
simpler but wastes more cycles on taken branches.

**Decision.** **All branches resolve in EX with static-not-taken
speculation.** IF and ID after a branch are fetched/decoded as if the
branch is not taken; if EX determines the branch is taken, IF and ID
are squashed and the pipeline restarts from the branch target. No
prediction structure; no early-resolve path.

**Rationale.** Static not-taken in EX is the simplest correct branch
policy and matches the MIPS R3000 reference design exactly. ~10% of
cycles in typical code are taken branches, costing ~0.3 CPI of bubble
(3 bubbles per taken branch × 10% = 0.3). That is below the
dependent-pair cost from
[Decision 4](#4-hazard-handling-strategy) and so is not the
binding CPI factor for gen2.

**Consequences.**

- Taken-branch flush penalty: **3 bubbles** (squash IF1, IF2, ID).
  This grew from the originally-planned 2 bubbles when
  [Decision 3](#3-pipeline-stage-count-and-shape) split IF into
  IF1+IF2 per [Decision 11](#11-bram-backed-caches-with-single-mem-stall).
- Untaken-branch cost: 0 cycles (speculation matched reality).
- JMP Rs and RTI are also EX-resolved with squash; same 2-bubble cost.
- No prediction table, no BTB, no branch history register in gen2.
- Gen2.5 will likely add a small static or bimodal predictor in IF to
  speculatively fetch the target on backward branches; the squash
  infrastructure built for gen2 is reused as the "mispredict recovery"
  path.

**Alternatives considered.** B-format early-resolve in ID (rejected
for gen2: requires a 1-bit flag-forwarding path from EX, which violates
the "no forwarding" rule of [Decision 4](#4-hazard-handling-strategy)).
Stall-on-branch with no speculation (rejected: every branch costs 2
cycles regardless of direction, worse expected CPI). Static taken
(rejected: most branches are forward and untaken; pessimal default).

---

## 6. Control architecture: pure hardwired, no microcode

**Date:** 2026-05-24

**Context.** Penumbra/1 is microcoded: each instruction trips a
sequence of micro-ops through the µROM, which directly drives the
single-cycle datapath. The natural question for Penumbra/2 is whether
to carry that mechanism forward: keep microcode for complex sequences
(exception entry, RTI, future hardware MUL/DIV); use multi-µop
expansion at decode; or eliminate microcode entirely. The answer
depends on whether the ISA actually has any multi-step operations that
benefit from a sequencer.

**Investigation.** The ISA was audited against the canonical spec
(`doc/system/architecture.md` § Exception Model and
`hw/microcode/microcode.uasm`). Result:

- Every "normal" instruction (R/L/M/B format, RDSPR/WRSPR, RDSYS/WRSYS,
  EI/DI, JMP, BL) is single-µop in nature: one ALU operation OR one
  memory access OR one register transfer.
- **Exception entry is NOT a stack push.** Hardware saves
  `PC → EPC`, `SR → ESR` (both SPRs, no memory); sets `S=1`, `I=0`;
  swaps SP bank to SSP. Then a *single* indirect memory load reads
  the handler address from `vector_table[vec_num × 4]` (physical,
  MMU-bypassed) and sets PC. Vector table is MIPS/68k-style — pointers,
  not instructions.
- **ERET** restores SR ← ESR and PC ← EPC. No memory access.
- Hardware MUL/DIV (gen2 scope per
  [Decision 1](#1-project-goals-and-non-goals)) is single-µop with
  multi-cycle EX stall. Single instruction issued by ID; the divmul
  unit iterates for ~34 cycles; then a single WB cycle writes both
  `Rd` (low half or quotient) and `Rdh` (high half or remainder) via
  the regfile's two write ports.

So the entire ISA is single-µop. The only multi-step event in the
whole architecture is "one indirect load through a vector table during
exception entry," which is structurally identical to a memory-indirect
jump.

**Decision.** Penumbra/2 has **no microcode subsystem**. Control is
hardwired per stage. The frontend gains a tiny vector-fetch FSM
(~3 states) to drive the indirect vector load during exception entry.
ERET is a single µop in EX with multi-write enable (writes SR, writes
PC, squashes IF/ID).

**Rationale.** Microcode's job in a single-cycle design is to *be* the
sequencer. In a pipelined design, the pipeline *is* the sequencer —
microcode's job is already taken. Keeping microcode would add: a µROM
module, a sequencer FSM, a `pipeline ↔ microcode mode` handoff
mechanism, and a microcode assembler — for the benefit of expressing
*one* logical operation (the vector indirect load) that an inline FSM
expresses in a dozen lines of SystemVerilog. The cost/benefit is
clearly against keeping microcode.

**Consequences.**

- **No new modules**: no `ucode_rom.sv`-equivalent, no
  `sequencer.sv`-equivalent, no `.uasm` source, no `uasm.py`-equivalent
  for penumbra2.
- **Existing gen1 microcode infrastructure remains gen1-only** —
  `hw/microcode/`, `hw/tools/uasm.py`, `ucode_rom.sv`,
  `sequencer.sv`. It does not migrate.
- **Per-stage combinational decoders**: IF, ID, EX, MEM, WB each have
  their own combinational decoder block that looks at the instruction
  in the stage register and emits stage-specific control signals.
  Shared decode helpers (immediate extension, format detection, op
  classification) live in a shared `decode.sv`.
- **Exception save-state pulse**: one cycle, parallel writes to EPC,
  ESR, mode bits, and the SP bank-swap latch. No FSM.
- **Vector-fetch FSM**: lives in `if_stage.sv`. States: `IDLE` (normal
  fetch), `VECFETCH_REQ` (drive MAR-equivalent to `vec_num << 2`, MMU
  bypass, memory request), `VECFETCH_WAIT` (waiting for memory
  response), then back to `IDLE` with PC ← loaded value.
- **ERET handling**: a drain-commit instruction (see
  [Decision 9](#9-drain-commit-primitive)). EX recognizes ERET, stalls
  upstream stages, lets MEM and WB drain (≤2 cycles), then commits
  directly from EX: writes `SR ← ESR` (with bank-swap side effect when
  `ESR.S=0`), signals frontend `redirect PC ← EPC`, squashes IF/ID.
  ERET does not advance past EX into MEM/WB — there is no GPR write
  or memory access.
- **WRSYS handling**: a drain-commit instruction with 1-cycle
  post-commit wait (see [Decision 9](#9-drain-commit-primitive)). EX
  recognizes WRSYS, stalls upstream, lets MEM/WB drain, then issues
  the sysreg sideband write from EX, then stalls upstream one more
  cycle to allow the target sysreg device to latch synchronously.
  This eliminates the need for explicit kernel barriers around
  sysreg-side-effect writes (TLB management, cache control, MMU
  enable, etc.). RDSYS does not need drain-commit — it's a normal
  pipelined read; the in-order pipeline naturally orders it after
  any prior WRSYS.
- **Privilege check**: combinational in ID. If the instruction is
  supervisor-only and SR.S = 0, the decoder raises an illegal/privilege
  exception flag, which propagates to commit.
- **EI delayed-by-one**: a one-bit shadow register in IF that delays
  `SR.I = 1` becoming visible by one fetch cycle. Same mechanism as
  gen1's `ei_shadow`. No µop involved.
- **MUL/DIV (gen2 scope per
  [Decision 1](#1-project-goals-and-non-goals))**: multi-cycle EX
  stall on a single µop using the divmul unit (per gen1's
  `divmul.md` spec). EX holds for ~34 cycles, back-pressuring
  upstream. At completion, the instruction commits in one WB cycle
  that writes both `Rd` via the main regfile write port and `Rdh`
  via divmul's dedicated second write port (see
  [Decision 4](#4-hazard-handling-strategy)). DIV-by-zero and
  DIVL overflow raise `VEC_ARITH` (vector 10).

**Alternatives considered.** Microcode trap-in for exception entry +
RTI (rejected: adds infrastructure for one logical operation; the FSM
+ inline ERET handler is smaller). Multi-µop expansion at decode
(rejected: most flexible but requires a smarter decoder for benefits
the ISA doesn't ask for). Pure microcoded pipeline (rejected: massive
overkill for a single-µop ISA).

---

## 7. Cache and MMU reuse strategy

**Date:** 2026-05-24

**Context.** Penumbra/1's L1 cache (`hw/rtl/soc/cache_vipt.sv`), L2
cache (`hw/rtl/soc/l2_cache.sv`), and MMU (`hw/rtl/mmu/`) are working
and tested. Penumbra/2 needs to integrate them into a pipelined
frontend (IF) and memory stage (MEM). The question is whether to
inherit them as-is, parameterize them, or fork.

**Decision.** **L1 caches are new (BRAM-backed) for Penumbra/2**
per [Decision 11](#11-bram-backed-caches-with-single-mem-stall) —
the gen1 `cache_vipt.sv` (distributed-RAM, combinational-on-hit)
cannot be reused as-is. **L2 (`l2_cache.sv`) and the MMU/TLB
modules (`hw/rtl/mmu/`) are reused unchanged.**

**Rationale.** The gen1 L1 cache was designed for a single-cycle
CPU that needed combinational hit response from distributed RAM.
Penumbra/2's pipelined design with BRAM-backed cache requires
registered output and a different hit timing contract; retrofitting
those onto `cache_vipt.sv` would amount to a rewrite anyway. Better
to build a new module that targets the pipelined contract from the
start.

The L2 (`l2_cache.sv`) sits on the shared Penumbra bus behind L1,
and its CPU-facing interface is "memory request in, memory response
out" — no assumptions about the L1's organization. Same for the
MMU/TLB modules (still distributed-RAM async lookup; the TLB
interface from gen2 IF1/MEM is identical to gen1).

**Consequences.**

- **New module: `cache_bram_vipt.sv`** (or similar name) lives in
  `hw/rtl/penumbra2/` initially (or `hw/rtl/soc/` if a parallel
  use case appears for gen1). VIPT same as gen1 (cache ≤ page size
  → no aliasing). Configurable size (start at 4 KB to demonstrate
  scaling, can grow). 1-cycle hit latency from registered address
  to data available combinationally during the next cycle.
- **The gen1 cache (`cache_vipt.sv`) stays alongside in gen1 builds**
  — both cores can compile and synthesize independently.
- **L2 cache (`l2_cache.sv`) is shared, unchanged.** Both cores
  instantiate it the same way.
- **MMU (`hw/rtl/mmu/`) is shared, unchanged.** TLB sysreg interface,
  pinned TLB, alignment check — all identical. Both cores' IF and
  MEM stages drive the MMU the same way.
- **The TLB stays distributed-RAM async lookup** (gen1-inherited).
  This is important: it means TLB output is available combinationally
  in IF1 (alongside the BRAM cache address being clocked), so the
  paddr_tag can be registered at end of IF1 ready for tag compare
  in IF2.
- **Per-line invalidation** is now meaningful (was reserved in gen1
  per memory). With BRAM-backed L1, per-line invalidate on TLB
  shoot-down avoids full-flush; not gen2 scope but the BRAM cache
  module should leave room for it.

**Alternatives considered.** Reuse `cache_vipt.sv` unchanged
(rejected per [Decision 11](#11-bram-backed-caches-with-single-mem-stall)
— the distributed-RAM combinational hit fundamentally conflicts
with the BRAM-backed pipelined design). Build a parameterized cache
that supports both organizations (rejected: parameterization at the
RAM-type level is essentially two modules in one — easier to keep
them as two modules with shared upper-level interface). Skip BRAM
for gen2 and inherit gen1 cache (rejected per
[Decision 11](#11-bram-backed-caches-with-single-mem-stall)
— forces 5-stage with low fmax ceiling and 1 KB cache size cap,
not the gen2 we're aiming for).

---

## 8. NetBSD kernel compatibility approach

**Date:** 2026-05-24

**Context.** The stated goal is "ideally the same NetBSD kernel
should work on both Penumbra/1 and Penumbra/2." Userspace must be
fully compatible; minor kernel-side deviation is acceptable if full
compatibility is too costly. The question is what mechanism gives
the kernel the information it needs to handle both cores.

**Decision.** Use `cpuid.sv`'s MODEL field to discriminate at boot.
Penumbra/1 reports `MODEL = 1`; Penumbra/2 reports `MODEL = 2`.
The kernel reads CPUID at boot and selects MD code paths
accordingly. ISA, SPR contracts, sysreg semantics, vector table
layout, and ABI all remain identical between cores; the kernel
should need to discriminate only on micro-architectural
differences (cache line size, fmax-derived timer calibration,
optional sysreg presence).

**Rationale.** The CPUID mechanism already exists for exactly
this purpose and is exposed via RDSYS to the kernel. Using it
keeps the ISA-level contract clean (both cores honor the same
instruction set; differences are advisory through CPUID) and
avoids any need for a per-core kernel binary. NetBSD ports
routinely discriminate this way (see `arm/cpu_*` paths in the
NetBSD tree).

**Consequences.**

- `hw/rtl/soc/cpuid.sv` will gain a new MODEL value (2) and be
  instantiated with that value in `ulx3s_top_penumbra2.sv`.
- The kernel's MD `cpu_attach()` (or equivalent) gains a `switch
  (cpuid.model)` block for any per-core differences. Until gen2
  finds a genuine difference that the kernel cares about, no
  switch is needed.
- Sysreg additions specific to one core (e.g., a future
  Penumbra/2 perfctr that doesn't exist on Penumbra/1) are
  permissible; the kernel checks CPUID before using them.
- If a TLB-or-similar deviation becomes truly necessary in gen2,
  it gets a CPUID-gated MD path and a note in this document.
  None foreseen at the time of writing.

**Alternatives considered.** Separate kernel binaries per core
(rejected: doubles the test surface, loses a key value of the
shared-ISA design). Detect at runtime via instruction probing
(rejected: CPUID is cheaper and exists for this purpose). Make
gen2 deviate from gen1 freely (rejected: violates the
"userspace fully compatible" requirement, and there's no
specific benefit driving deviation).

---

## 9. Drain-commit primitive

**Date:** 2026-05-24

**Context.** A pipelined design needs a mechanism for instructions
whose effects must be ordered with respect to (a) older in-flight
instructions whose architectural completeness affects the
instruction's semantics, and (b) subsequent instructions that observe
the side effects of the ordered instruction.

Two Penumbra/2 instructions need this ordering:

- **ERET** restores `SR` (architectural state including mode bits)
  and redirects `PC` to `EPC`. If ERET committed in EX immediately, a
  fault detected on an older insn one cycle later (e.g., MMU fault on
  a load in MEM) would save `ESR ← SR_post_ERET`, which is the wrong
  state — the fault was actually taken under the pre-ERET SR.
  Additionally, the common kernel sequence `WRSYS TLB_PTE; ERET`
  requires that the new TLB entry is observable by the user-mode IF
  fetch after ERET — which requires the WRSYS's sideband write to
  have been latched by the TLB device before ERET's PC redirect takes
  effect.

- **WRSYS** issues a sysreg sideband write whose effect is observable
  by subsequent instructions (TLB writes change MMU lookups; cache
  control writes change subsequent memory access behavior; MMU enable
  bits change translation). A naively-pipelined WRSYS that commits at
  MEM might race with the next instruction that depends on the new
  state, especially if the sideband device latches synchronously on
  the next clock.

A general primitive that handles both is cheaper and clearer than
ad-hoc per-instruction ordering machinery.

**Decision.** Introduce a hardware primitive called **drain-commit**.
An instruction tagged as drain-commit:

1. **Stalls in EX upon entering**, back-pressuring upstream stages
   (ID and IF freeze with their current contents).
2. **Lets MEM and WB drain naturally** — each older instruction
   continues through its remaining stages and commits in ≤2 cycles.
3. **Commits directly from EX** once MEM and WB hold bubbles
   (writes its outputs to whatever architectural state it changes:
   SR, PC, sysreg sideband, etc.).
4. **Optionally stalls upstream for `N` additional cycles** (where
   `N ∈ {0, 1}` in gen2) to allow external side effects to propagate
   before subsequent instructions advance into observation-eligible
   stages.

The instruction itself never advances past EX into MEM/WB —
drain-commit instructions are not present in MEM or WB during their
commit cycle.

**Two variants in gen2:**

| Instruction | Post-commit wait | Why |
|-------------|------------------|-----|
| ERET | 0 cycles | SR/PC change is internal to the CPU, observable the same cycle |
| WRSYS | 1 cycle | Sysreg sideband write must latch in the target device (synchronous, next-clock) before subsequent insns can observe the new state |

The variants share all mechanism; they differ only in a 1-bit
"post-commit wait" decoder flag.

**Rationale.** The alternative — carrying pending-effect values
through MEM/WB pipeline registers for drain-commit instructions —
would allocate 64+ bits per pipeline register that are used only by
ERET (and similar) instructions. By committing in EX after the drain,
those bits never need to ride downstream, and the instruction's
commit happens at a single well-defined point in the pipeline.

The alternative for WRSYS — requiring the kernel to insert explicit
barrier instructions after every sysreg-side-effect write — would be
invasive (changes to every WRSYS call site) and easy to get wrong
(missing barriers cause subtle race conditions). Hardware enforcement
is much harder to forget.

**Consequences.**

- **Per-instruction cost.** Drain-commit costs ≤2 cycles of pipeline
  drain plus the post-commit wait. Total: 2 cycles per ERET, 3 cycles
  per WRSYS. Total syscall overhead from WRSYS draining is ~40 cycles
  (~10 WRSYS per syscall × 4 cycles), which is ~4% of typical syscall
  cost — acceptable.

- **Decoder.** A single bit in the control bundle marks an
  instruction as drain-commit; a second bit marks the post-commit
  wait variant. Both are set by the ID-stage combinational decoder
  from the opcode.

- **EX-stage FSM.** A small FSM in EX handles the drain-commit
  protocol: enter `DRAIN` state when a drain-commit insn enters EX;
  count down a "downstream non-bubble count" each cycle; commit when
  count reaches 0; for variants with post-commit wait, hold upstream
  stall for one more cycle before releasing.

- **Pipeline register width.** No new bits needed in MEM/WB pipeline
  registers — drain-commit insns never enter those stages.

- **Scoreboard interaction.** Drain-commit insns *can* write GPRs (in
  principle), but neither gen2 user does — ERET writes SR + PC,
  WRSYS writes sysreg-sideband. So the scoreboard is unaffected by
  drain-commit in gen2.
- **SR.S quiescence for the scoreboard.** ERET and WRSPR SR are both
  drain-commit, which means no in-flight instruction can be in the
  process of changing `SR.S` while a later instruction is being
  decoded. This is load-bearing for the unified physical scoreboard
  in [Decision 4](#4-hazard-handling-strategy): the decoder
  reads `SR.S` to map architectural `R14` to physical USP or SSP, and
  that mapping must remain correct for the duration of the
  instruction's pipeline residence. Drain-commit on mode-changing
  instructions provides that guarantee for free; without it, the
  scoreboard would need a more complex re-mapping mechanism.

- **Initial users.** ERET, WRSYS.

- **Future users.** Any new SYNC/FENCE-style instruction (none
  planned for gen2, but the mechanism is in place). Potentially WRSPR
  if SPR ordering ever becomes a concern (currently single-CPU and
  SPR reads happen via the same EX stage that performs writes, so
  ordering is naturally serial — not currently needed).

**Alternatives considered.**

- *Carry pending-state through pipeline registers* (rejected: 64+
  bits per stage of state used only by rare instructions; constant
  fmax tax on the common case for the benefit of a rare one).

- *ERET-only stall mechanism, no WRSYS handling* (rejected: kernel
  would need explicit barriers after every sysreg-side-effect WRSYS,
  which is invasive and prone to omission. Documented bug class to
  avoid).

- *Hardware ordering via explicit pipeline drain on every instruction
  with any side effect* (rejected: pessimal for performance; ALU ops
  also have side effects (flags) but don't need cross-instruction
  ordering; specific instructions only need this).

- *Per-instruction custom drain logic in EX* (rejected: same
  mechanism repeated for each ordered instruction; primitives are
  cheaper than instances).

---

## 10. Stall propagation policy (back-pressure)

**Date:** 2026-05-24

**Context.** In a pipeline, multiple stages can stall in the same
cycle (e.g., IF1/IF2 taking an I-cache miss while MEM is mid-D-cache
fill; ID scoreboard-stalling while EX is mid drain-commit; divmul
busy in EX while ID would have stalled anyway on the divmul's
pending writes). The "stall propagation policy" decides what
happens to other stages when one stalls: do they freeze together,
propagate stall pressure only upstream, or stall independently?

Three textbook options exist, with different complexity and
throughput characteristics:

1. **Whole-pipeline freeze.** Any stall holds every pipeline
   register. The entire pipeline advances together or not at all.
   *Has a deadlock problem with scoreboard hazards: if a stalled
   stage holds downstream too, the producer the stall is waiting
   for can never commit.*
2. **Back-pressure (stall-ahead).** A stall in stage N holds N's
   input register and propagates "I can't accept" upstream
   (stages 0..N also hold). Downstream of N drains naturally
   (bubbles fill behind).
3. **Independent per-stage stalls with bubble injection.** Each
   stage stalls itself and injects bubbles downstream when stuck;
   upstream-vs-downstream-stall isolation is full.

**Decision.** **Option 2 — back-pressure for gen2.**

(An earlier draft of this decision picked "whole-pipeline freeze"
as the simplest framing. Writing the timing examples in
[pipeline-stages.md](./pipeline-stages.md) surfaced the deadlock
issue: under literal whole-pipeline freeze, a scoreboard-stalled
consumer in ID would hold its producer in EX/MEM/WB too, and the
producer would never commit, leaving the scoreboard pending
forever. Back-pressure resolves this naturally — it's still simple,
still correct, still verification-friendly, and is what real
implementations do.)

**Rationale.** Back-pressure is the standard textbook formulation
and the simplest *correct* policy that handles all stall sources
uniformly:

- **A stalled stage holds its input register and its own internal
  state.** The next-upstream stage's output register also holds
  (back-pressure cascade), and so on up to IF1.
- **Stages downstream of the stall advance normally.** They were
  going to drain anyway; the stall doesn't change that. If a stage
  downstream has no upstream work to receive (because upstream is
  held), it sees a bubble.
- **No deadlock.** A scoreboard-stalled consumer in ID holds IF1,
  IF2, and ID; the producer in EX/MEM/WB drains normally, commits
  at WB, sets the scoreboard, releases the stall, and ID issues
  on the next cycle.
- **Waveform debugging is still very readable.** "Stage N is held;
  upstream is held; downstream is draining" is the same pattern
  across every stall type — the only thing that varies is *where*
  N is. Easier to understand than "everything held" but only
  slightly.
- **Verification scales linearly.** Each stall source is tested
  individually; the back-pressure cascade is one piece of logic
  (the "stage_N stalls if stage_N+1 holds OR stage_N's own stall
  condition") tested once.

Throughput is naturally better than whole-pipeline freeze (which
forces downstream of a stall to also wait, even though it has no
reason to). On a typical NetBSD workload the difference is in the
single-digit percentage range — not the deciding factor, but a nice
side effect of doing the simpler thing correctly.

**Consequences.**

- **Per-stage stall logic.** Each pipeline register's
  clock-enable is `(this_stage_can_accept) && !(this_stage_stall)`.
  `this_stage_can_accept` is back-pressured from downstream;
  `this_stage_stall` is the stage's own condition (cache miss,
  scoreboard pending, drain-commit drain, divmul busy, etc.).
- **A bubble flows downstream of any stall.** When stage N stalls,
  stage N+1's input register gets a bubble next cycle (because N
  is producing nothing useful).
- **Pipeline-register layout is unchanged.** Same registers, same
  fields; only the clock-enable derivation changes.
- **Waveform debugging is uniform.** "Stalled stage and everything
  upstream hold; everything downstream drains." Same shape for
  cache miss, scoreboard, drain-commit, divmul, vector fetch.
- **CPI impact.** None compared to a hypothetical whole-pipeline
  freeze that wasn't deadlocked — back-pressure is *better* than
  freeze on throughput, not worse.

**Alternatives considered.**

- *Whole-pipeline freeze (option 1)* — rejected: deadlocks on
  scoreboard hazards. Would require carving scoreboard out as a
  separate "not really a stall" mechanism, which is just
  back-pressure with extra steps.

- *Independent per-stage with bubble injection (option 3)* —
  rejected: more complex than back-pressure for marginal extra
  throughput (lets downstream-of-stall continue accepting upstream
  bubbles, which back-pressure also achieves). If we ever need
  this, it's gen3 territory.

- *Hybrid: back-pressure with a one-deep bubble slot between
  ID and EX* — discussed during planning; rejected for gen2 as
  more complex than plain back-pressure with no benefit at the
  gen2 CPI target.

---

## 11. BRAM-backed caches with single-MEM-STALL

**Date:** 2026-05-24

**Context.** Penumbra/1's L1 cache (`cache_vipt.sv`) uses
distributed-RAM storage with combinational hit path — a single-cycle
read returns valid data, tags, and hit signal all in one async
cone. This was the right choice for a single-cycle microcoded CPU
under the discrete-74xx feasibility constraint (distributed RAM
maps directly to 74xx async RAM chips).

Penumbra/2 abandons the discrete constraint and aims for higher
fmax and a larger cache. Inheriting gen1's cache hits two ceilings:

1. **Cache size.** Distributed RAM is LUT-implemented. ECP5-85F has
   ~84k LUT4 equivalents but only a fraction can be used for RAM
   before logic gets squeezed. 1 KB is reasonable; 4 KB-8 KB starts
   to hurt; 16 KB is impractical.
2. **Fmax.** The combinational hit path (distributed-RAM async read
   + tag compare + data mux + TLB lookup) is ~10-15 ns. In gen1
   this lives inside a longer single-cycle critical path. In a
   pipelined gen2 it becomes its own stage, but it still caps the
   stage at ~70 MHz before routing overhead, which is the practical
   ceiling.

Industry-standard FPGA softcores (VexRiscv, Rocket, Ibex,
CV32E40P, BOOM, NEORV32) universally use **BRAM-backed caches with
registered output** — typically a 2-cycle fetch latency absorbed
into the pipeline via either an extra IF stage or a prefetch
buffer. This unlocks larger caches (BRAM is "free" in the sense
that the FPGA has many EBR blocks that mostly sit unused) and
higher fmax (BRAM output is ~3 ns max vs ~5-6 ns for distributed
RAM cascade).

**Decision.** Penumbra/2 uses **BRAM-backed L1 caches** for both
instruction and data. The pipeline accommodates the BRAM latency
asymmetrically:

- **I-cache: split IF into IF1 + IF2.** IF1 drives the BRAM address
  (and TLB lookup); IF2 receives the BRAM output, performs tag
  compare and fault detection. Every fetch costs 2 cycles, but the
  pipeline absorbs this naturally — only branch flushes and
  exception drains pay an extra bubble compared to single-cycle IF.
- **D-cache: keep MEM as a single stage; STALL for 1 cycle on every
  cache access.** MEM drives the BRAM address at cycle T and asserts
  the existing STALL signal; at cycle T+1 the BRAM output is ready,
  tag compare and hit detection happen, STALL deasserts, and the
  instruction advances to WB. ALU/branch/sysreg-internal insns pass
  through MEM in 1 cycle (no STALL), so non-memory code is not
  penalized.

**Rationale.** The asymmetric treatment of IF vs MEM bills the
cost where it matters:

- **I-fetch is on every cycle.** Splitting IF gets the BRAM cost out
  of every instruction's critical path. The branch-flush penalty
  growth (2 → 3 bubbles) is the only cost — paid only on taken
  branches (~10% of cycles), so +0.1 CPI.
- **D-access is only on loads/stores** (~25-30% of insns). A split
  MEM would force every instruction to pay an extra pipeline stage,
  even though most don't touch D-cache. The single-MEM-STALL
  approach bills the 1-cycle BRAM latency only to memory ops, so
  the net cost on mixed workloads is lower (+~0.25 CPI for load
  stalls vs +0.5 CPI from blanket extra-stage cost on a 7-stage
  pipeline).
- **Reuses existing STALL mechanism** — no new pipelining
  infrastructure on the D-side. Cache misses already stall MEM;
  cache hits just stall for 1 cycle instead of 0.
- **Per L2 workload-sensitivity profile** (memory file
  `project_l2_workload_sensitivity`): real Penumbra workloads are
  I-fetch-latency-bound (kernel syscall paths), not D-throughput-
  bound. Memcpy-style workloads are SDRAM-bandwidth-bound regardless
  of L1 D-cache organization. Optimizing for memcpy back-to-back
  loads is not where the leverage is.
- **Aligned with reference designs.** VexRiscv, Ibex, Rocket all
  use BRAM with a similar shape (though they typically split MEM
  too; the single-MEM-STALL is a Penumbra-specific simplification
  enabled by the asymmetric I/D usage frequency).

**Consequences.**

- **New `cache_bram_vipt.sv` module** (or similar). Lives in
  `hw/rtl/penumbra2/` initially. VIPT (cache ≤ page size precondition
  inherited from gen1). Configurable size — start at 4 KB to
  demonstrate BRAM scaling; growable to 8/16 KB if synthesis
  budget allows. Tag + data + valid arrays in BRAM with registered
  output.
- **Gen1's `cache_vipt.sv` is unchanged and continues to serve
  Penumbra/1.** Both cache modules coexist in the tree.
- **Pipeline is 6 stages** (IF1/IF2/ID/EX/MEM/WB) — see
  [Decision 3](#3-pipeline-stage-count-and-shape).
- **Branch flush penalty: 3 bubbles** (IF1/IF2/ID) — see
  [Decision 5](#5-branch-resolution-policy).
- **Load-use distance: 4 cycles** under pure-stall (load completes
  WB 1 cycle later than ALU due to MEM-STALL; dependent ALU stalls
  4 cycles in ID) — see [Decision 4](#4-hazard-handling-strategy).
- **Back-to-back load IPC: 0.5** (each load takes 2 cycles in MEM
  due to STALL). Memcpy/memset performance penalty vs split-MEM;
  accepted for gen2.
- **Exception drain cost: 2 cycles** (drain MEM + WB). Drain-commit
  ERET and WRSYS pay the same as in the originally-planned 5-stage
  design — no growth from the IF split because IF1/IF2 are upstream
  of EX.
- **Fmax ceiling lifted to ~50-60 MHz** territory (limited by EX
  ALU compute, not cache). Gen2.5+ ALU optimizations could push
  further.
- **TLB stays distributed-RAM** (gen1-inherited). Its async lookup
  fits naturally in IF1, with the paddr_tag output registered into
  IF1/IF2 register at end of IF1. Likewise for D-side in MEM.
- **Same kernel works on both cores** because all cache organization
  is invisible to software — cache size, line size, associativity
  differences are readable via the existing `SYSREG_CACHE_INFO`
  register (`cache_vipt.sv:159-167`), so the kernel can adapt
  cache-line-size-dependent code (icache flush, dma sync) at boot.

**Alternatives considered.**

- *Inherit gen1's distributed-RAM cache* (rejected: caps cache at
  ~1 KB and fmax at ~30 MHz — defeats the gen2 goals).
- *Split MEM into MEM1/MEM2 (7-stage pipeline)* (rejected: forces
  ALU/branch insns to pay an extra stage they don't benefit from;
  grows branch-flush penalty to 4 bubbles and exception-drain to
  3 stages; benefits only memcpy-class workloads which are
  bandwidth-bound regardless).
- *Build a generic configurable cache that supports both
  distributed-RAM and BRAM backing* (rejected: parameterization
  across fundamentally different timing contracts is essentially
  two modules in one — cleaner to keep them as separate modules
  with a shared upper-level interface).
- *Use a prefetch buffer instead of split IF* (deferred: this is
  the VexRiscv `IBusSimplePlugin` approach. Adds buffer state,
  prefetch logic, and bypass paths. Worth considering as a
  gen2.5+ optimization when branch prediction lands; not gen2).
- *BRAM with output register (REGMODE_A=OUTREG) for 3-cycle
  IF* (rejected: needlessly grows IF to 3 stages without benefit —
  the registered-output mode is for closing timing in much larger
  systems; we don't need it).

---

## Project-wide conventions (referenced here)

The following conventions are project-wide and not gen2-specific.
They are recorded outside this document to keep gen2 decisions
focused.

- **Penumbra versioning naming** (penumbra1/penumbra2 in paths) —
  see [Decision 2](#2-rtl-and-doc-organization-shared-vs-forked)
  for how it applies to gen2; the convention itself is general
  and was chosen alongside the gen2 layout decision.
- **Diagram conventions** (Mermaid for block/state/sequence
  diagrams, Markdown tables for pipeline timing) — applies to all
  new documentation under `doc/`, not just gen2.
- **Commit message format** (`<subsystem>: <imperative>`) —
  documented in the root `CLAUDE.md`.
- **RTL coding standards** — `doc/internals/coding-standards.md`,
  applies to all RTL including penumbra2.

---

## Future entries

This document grows as Penumbra/2 develops. Add a new entry whenever
a non-obvious design call is made — pipeline register layouts, stall
propagation policy, scoreboard width decisions, regfile port
arrangements, fault propagation routing, perfctr additions,
debug/trace ports, etc. Keep the entry structure consistent: Context,
Decision, Rationale, Consequences, Alternatives considered.

Past entries are not edited when they are later refined; instead, a
new entry references the older one and explains the refinement. This
preserves the design narrative.

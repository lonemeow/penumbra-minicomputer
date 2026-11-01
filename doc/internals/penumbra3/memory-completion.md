# Penumbra/3 — Memory access completion & back-end stall

> **Applies to:** Penumbra/3 · pipelined core (in design).

This specifies how every data-memory access *completes* — the back-end
stall that gates issue on a **registered** completion bit instead of the
live cache `busy`, and the path each access takes to its architectural
effect: a load to its register result, a store to memory. The registered
load-completion back-end is the single highest-value change in the gen3
plan (it deletes gen2's fmax floor); stores ride the same single-outstanding
machinery, so they are specified here too.

It builds on the [timing outline](./timing-outline.md) (the back-end stall
section), the [MEM1/MEM2 stage](./mem-stage.md) (which produces the verdict
this consumes), and the [core-to-uncore port](./uncore-port.md) (which
every access that leaves the core crosses). The full forwarding network
is a separate spec; this doc covers the load path, the store path, and the
issue gate they feed.

## The gen2 floor this deletes

gen2 stalled on the **combinational** D-cache `busy`: `D-cache tag compare →
busy → stall network → ID issue` (and the same cone reached the MMU
verdict). That is one combinational path the placer spreads across the die
(~29 ns, ~34 MHz) — a structural floor, not a placement problem, and the
reason gen2 was abandoned. Every gen3 back-end rule below exists to make
sure no such path is ever formed.

## The rule

**No combinational path runs from a cache hit/`busy` or a TLB verdict into
issue or the MMU.** The memory verdict (formed in MEM2, see
[mem-stage](./mem-stage.md)) only ever *writes a flop*; issue only ever
*reads flops*. The connection between them is always a register.

## Load hit — a registered load-use hazard

A load that hits needs no pipe stall; it creates only a load-use hazard for
a dependent issued too close behind. That hazard is resolved against
**registered** state:

- Issuing a load sets its destination's scoreboard bit (a flop).
- A dependent reads that bit at issue; if the value isn't yet forwardable
  (from the MEM2/WB register), it stalls at issue — reading a flop, never
  the cache verdict.
- The bit clears on completion (the cycle the hit resolves, registered).

A load's value forwards from the MEM2/WB register, so a dependent issued
directly behind it waits 2 cycles. MEM2 forwards only results computed
before it: a load's word resolves during MEM2, behind the TLB and cache
block RAM reads and the tag compare, and forwarding it from there would
carry that cone on through EX's operand mux and ALU in the same cycle.
Sysreg reads, which resolve in MEM2 the same way, forward from MEM2/WB
too.

## Load miss — hold-and-complete

A miss does **not** re-run MEM1 each cycle. It is *held* and *completed*:

```mermaid
graph LR
    M2[MEM2 miss] --> HB[one-entry<br/>hold buffer]
    HB --> LP[mem_pending_q<br/>set]
    HB --> FILL[fill launched once<br/>registered txn]
    FILL --> BUS[penumbra3_bus_master<br/>line fill]
    BUS --> CMP[registered completion<br/>{data | fault}]
    CMP --> WB[held load -> WB,<br/>mem_pending_q clear]
```

- **One-entry hold buffer** parks the missing load (single-outstanding ⇒ one
  entry). MEM1 is not replayed.
- **`mem_pending_q`** (a flop) is the pipeline gate. The pipe holds via
  back-pressure off *that flop*, not off the live `busy` — this is the only
  change vs gen2's load path. **Hold-the-pipe is the design point, not a
  placeholder.** Issuing independent work past a miss (non-blocking loads)
  needs multi-outstanding tracking and a completion-driven wakeup of waiting
  dependents — a match/CAM landing squarely on the issue cone this design
  works to keep shallow. It trades the fmax gen3 exists to protect for IPC
  only on miss-heavy code, and going multi-outstanding would also cost the
  precise-fault property below. It is out of scope by design, not deferred.
- **The fill is requested once,** as a line read on the
  [core-to-uncore port](./uncore-port.md): the hold buffer's registers hold
  the request up until it completes, and the
  [bus master](./overview.md#fork-boundary) carries it out as one registered
  line transaction (validated in P0.4).
- **The line is installed as the request completes,** into the way the
  miss parked with, unless the read faulted or the L1 is disabled. It runs
  from the uncore's line register into the L1 and never through the core.
- **Completion is a registered event** in the cycle after the request
  completes. It delivers the data to the held load's register, or takes the
  [bus fault](#fault-precision), and retires the instruction.
- **The launch side resumes in that same cycle,** with one exception. The
  L1 shows a line to a lookup it already holds two cycles after the
  install, and the instruction held in MEM2 behind the miss holds such a
  lookup. The L1 says so in the install's cycle: it reports an install
  into the set its current lookup reads. When it does, and the held
  instruction is a load or a store, `mem_pending_q` clears a cycle later.
  The completion unit reads only that report, never the L1's sets or
  ways. A read that installed nothing, an install into another set, and
  a held instruction that reads no lookup all resume with completion.

## Stores — write-through, write-no-allocate

A store has no register result, so it never creates a load-use hazard and
never sets a scoreboard bit. Its architectural effect *is* the memory write.

The write policy is **write-through, write-no-allocate (WnA)**:

- **Cacheable store, L1 hit** — the L1 line is updated in place **and** the
  datum is written through to the bus side. The L1 write lands a cycle
  after MEM2, from registers, so the hit never drives a block RAM's write
  enable: the cache merges the store's bytes over the word its own lookup
  read and writes the whole word. The block RAM shows a written word two
  cycles after the write; a lookup that resolves before then takes the
  word from the cache's write register, or from a copy one cycle behind
  it, so a load directly behind a store reads it without a stall.
- **Cacheable store, L1 miss** — WnA: L1 is left untouched (no allocate); the
  datum is written through to the bus side only. A later load to that line
  misses L1 and fills from L2/memory, which already holds the completed store
  — so **no store-to-load forwarding is needed**, a direct consequence of the
  single-outstanding hold-and-complete drain below.
- **Uncached store** — written through to the bus side. It finds no line
  in L1: software invalidates the caches before it makes a cached page
  uncacheable ([MMU, "Uncached Access"](../../system/mmu.md#uncached-access)),
  so the L1 checks no cacheability on a hit.

Every store therefore produces one off-core write transaction: a bus-master
**single beat** carrying the byte-enable mask, so a sub-word store writes
only its lanes. The line-write transaction is reserved for whole-line
write-back evictions, never write-through.

The write rides the **same single-outstanding hold-and-complete path** as a
load miss: the store parks, the write is launched once as a registered
transaction, the launch side freezes on `mem_pending_q`, and a registered
completion releases it. A store writes back no register — completion only
releases the freeze and resolves the fault (below).

What parks is the access, and its instruction need not wait with it. A
cacheable store retires as its write parks, because nothing the write can
report comes back to the instruction
([fault precision](#fault-precision)). An uncached store retires with its
completion, as a missed load does.

Decoupling the pipe from the store drain — a store/write buffer, or absorbing
stores in a write-back L2 so only evictions reach memory — is an orthogonal
throughput layer, described in the
[store-traffic strategy](./overview.md#store-traffic-strategy--store-buffer-first-write-back-l2-only-if-it-earns-it).
It does not change this completion contract.

## One completion unit, four access shapes

A single single-outstanding unit parks any access that does **not** resolve
as a cache hit in MEM2 and drives it to completion (a cacheable hit —
**including a sub-word hit** — is served in MEM2 by byte extraction and never
enters this unit). Single-outstanding falls
out of the one `PENDING` state; the launch-side freeze serializes the next
memory op behind the in-flight one, so the bus master never sees two
transactions and no cross-unit interlock is needed. Per-access behaviour
reduces to three orthogonal choices — transaction shape, writeback, and
fault precision:

| Access | Bus transaction | Writeback | Fault |
|---|---|---|---|
| Cacheable load miss | line read | value (sub-word extract) | precise |
| Uncached load | single read | value | precise |
| Uncached store | single write (+byte-en) | none | **precise** |
| Cacheable store write-through | single write (+byte-en) | none | **async (sticky)** |

Each row is one transaction of the
[core-to-uncore port](./uncore-port.md#the-four-transactions): the line
read, the word read, the word write and the cacheable write.

## What the pipe gate freezes

`mem_pending` is back-pressure with a boundary, not a global freeze: it holds
the launch side and lets the commit side drain. It is set by an outstanding
load *or* store; a store pends identically to a load (only its completion
differs — no register writeback).

- **Held while an access is pending:** MEM1, the MEM1/MEM2 register, the
  [address-translation launch](./mem-stage.md), and issue (a dependent stalls
  on the scoreboard, see [the issue gate](#the-issue-gate)). The pending
  access and every younger instruction hold in place. The instruction held
  in MEM2 keeps its verdict and does nothing with it: its commit, a store's
  cache write and a miss of its own all happen in the one cycle it leaves
  MEM2, which is also the rule for a slot held there by the commit stage
  or squashed by a fault.
- **Not held:** the MEM2/WB register. It carries instructions *older* than the
  pending access to the commit point, and an older instruction must retire
  while the access waits. A pending access drives that register to bubbles,
  and a held load reaches a register through the completion path above,
  never through MEM2/WB, so draining the register is always correct. (A
  store reaches no register at all; a cacheable store passes through this
  one as it parks, ahead of its write.)

The MEM2/WB register's only back-pressure is a genuine downstream stall: the
commit stage holding for a multi-cycle writeback (the divmul dual-register
retire, which writes its two destinations through the single regfile port over
two cycles). Folding `mem_pending` into the MEM2/WB freeze re-presents that
already-committed writeback as a fresh slot and retires it twice — the two
freeze sources stay separate.

A **drain-commit** (ERET, WRSYS, EI, DI) commits from EX, ahead of the back
end, so the freeze alone does not order it: EX holds one until nothing older
is unfinished. That covers MEM2, WB, a pending access, and that access's
completion cycle, which retires an instruction of its own. A pending access
may yet fault, and nothing younger may have taken effect by then. A cacheable
store's write counts as well, though its instruction has retired, so a
sysreg write stays ordered behind it.

## Fault precision

A memory fault is taken on the held, oldest-in-flight access. Whether it is
*precise* depends on whether that access's effect may be deferred:

- **Precise — squash-younger + redirect, identical to a MEM2 alignment
  fault:** any **load** bus fault, any **uncached store** bus fault, and all
  MMU-born faults (alignment, TLB miss, protection). The alignment/TLB/permission
  faults resolve at MEM2 *before* any bus transaction, so they are precise by
  construction. A load and an uncached store are not deferrable — the load's
  consumer waits on the value, and an uncached (device) store is ordered and
  unbuffered — so each is the oldest in flight when its transaction resolves.
- **Asynchronous — recorded in a sticky status bit, never a per-store trap:**
  a **cacheable store** bus fault. By contract a cacheable store's write-back
  may be deferred arbitrarily (a write-back cache, a store/write buffer): the
  storing instruction is permitted to retire before the write reaches memory,
  so a bus fault on it cannot be tied to that instruction. Writing the
  contract to the *weakest* guarantee the cache is allowed to make is what
  keeps a write-back L2 a transparent throughput upgrade rather than an ABI
  change.

The whole carve-out is one qualifier, registered with the parked access:

```
fault_precise = ~(is_store & cacheable)
```

The same qualifier says where the instruction retires: a cacheable store as
its write parks, every other parked access with its completion.

A precise fault reuses the existing data-fault vector and `FADDR`/`FSTAT`;
the asynchronous path sets the sticky bit only. A cacheable store targets
memory-backed storage, which answers, so the sticky bit records an otherwise
unreachable error rather than dropping it silently — the surfacing of that
bit (an asynchronous-exception vector) is a separate concern, not part of the
synchronous completion contract here.

The instruction has left the pipe by the time a bus fault arrives, so the
parked access keeps what the fault reports: the PC for `EPC`, the virtual
address for `FADDR`, and the mode and access type for `FSTAT`.
The completion commits the fault in WB's place, with the same flush and
save-state. Everything issued after the access is squashed with nothing left
to clear its pending bit, so any fault commit clears the scoreboard: the
faulting instruction is the oldest in flight, so every bit still set belongs
to something squashed.

Two invariants hold by construction and are asserted (Verilator `--assert`):

- a completion that delivers a **precise** fault is the oldest in flight (the
  single-outstanding invariant), so the squash/redirect is precise;
- `is_store & cacheable` is the **only** access class that completes
  asynchronously — every other completion is precise.

A faulting line fill installs nothing (the line stays invalid); the fault
rides the completion, transient, not cached.

## Write policy is a swappable knob

Write-through / write-no-allocate is the **default** L1-D policy, not a fixed
property of this contract. Everything above — the
[fault-precision carve-out](#fault-precision), single-outstanding
hold-and-complete, and the verdict-in / completion-out core seam — is
**policy-independent**, so write-back and write-allocate drop in without a core
change. The point is to let the cost vs complexity/fmax trade-offs between them
be *measured* by swapping the memory-hierarchy side alone.

What makes that hold is the [deferral principle](./overview.md#build-plan) made
structural: **write policy lives entirely on the bus side of the MEM2 verdict**
— L1 storage, the fill/completion sequencer, and the bus master — never in the
core's verdict / freeze / issue cones. The core sees only a registered hit/miss
verdict in and a registered completion out; it never learns which policy
produced them, so experimenting with policy cannot perturb the fmax the
[structural probes](./phase0-probes.md) prove.

The policy is two orthogonal knobs, each defaulting to the cheap leg:

| Knob | Default | Alternative | What the alternative adds — all bus-side |
|---|---|---|---|
| **Store-miss allocate** | no-allocate — write the datum through to memory only | write-allocate — line fill + merge the store lanes + install | reuses the load-miss line-fill path plus a write-merge step |
| **Store-hit write** | write-through — update L1 *and* memory | write-back — update L1 and mark dirty; memory is written at eviction | per-line dirty bits + an eviction-then-fill sequence in the sequencer (L1 stays passive storage); the bus master's line-write eviction path (already present, P0.4-validated) |

Two invariants every policy must preserve — they are what keep the doors open:

- **Cacheable-store bus faults stay asynchronous.** A deferring cache
  (write-back, or a store buffer) may push the write arbitrarily far past the
  instruction, so the contract is written to that weakest guarantee.
  Write-through *could* deliver these precisely, but does not — a stable
  contract across policies is worth more than precision no policy is allowed to
  rely on.
- **Single-outstanding holds.** An eviction and an allocate-fill are each one
  transaction, so the precise-fault property for loads and uncached stores is
  unchanged.

Only one knob reaches beyond the bus side: **write-back changes coherence with
other hardware.** Write-through keeps memory current, so hardware that reads
memory needs no help; write-back leaves dirty lines off-memory, so the kernel
must flush before other hardware reads memory and invalidate before it writes —
the per-line cache-maintenance and full-flush hooks the platform reserves for
exactly this case. That cost is a property of the *policy*, not of this
completion contract.

The phased rollout that exercises these knobs — write-through with a store
buffer first, write-back only if measurement asks for it — is the
[store-traffic strategy](./overview.md#store-traffic-strategy--store-buffer-first-write-back-l2-only-if-it-earns-it);
this section specifies the seam it plugs into.

## Single-beat transactions — the same completion path

A transaction that is not a cacheable line read is a single beat on the bus
master's non-line path, and it rides the same hold-and-complete machinery as a
line fill — held, oldest, waiting for `{data | fault}` (a read) or the write
acknowledgement (a write). Two access classes use it:

- **Uncached accesses** (load or store) — no line, no caching; transactional
  delivery of the one word.
- **Cacheable store write-throughs** — the write reaches memory as a single
  beat carrying the byte-enable mask (so a sub-word store writes only its
  lanes), while the L1 copy is updated in place on a hit.

**Sub-word is orthogonal to this — it is not a cache bypass.** A cacheable
sub-word **load** is served from L1 on a hit (the cache returns the full word
and `byte_ext` extracts the lanes, so the strlen/byte-stream hot path stays at
cache speed) and fills a whole **line** on a miss — never a single beat. Only
the **store** side carries a sub-word mask onto the bus, because a
write-through must merge into memory; every read pulls a full word — from the
cache or a line fill — and extracts locally.

**Boot device-probing depends on the uncached read path:** the probing load is
the oldest, held, and a bus fault from a missing/erroring device is delivered
to it precisely, so the probe handler runs. An uncached store's bus fault rides
the same precise delivery; precise read faults alone suffice for probing, and
the write case comes along on the one qualifier above.

## The issue gate

Issue computes `can_issue` from registered inputs only: the decoded source
register numbers (registered out of the pre-decode FIFO) compared against
the scoreboard's pending bits (flops), qualified by what the forwarding
network can supply this cycle. This compare is reg#-equality on already-
registered fields — 1–2 LUT levels, the shape every surveyed core uses,
never a long path. It is the cone P0.1 and P0.2 share, and the natural place
for a focused contribution when the RTL lands.

ALU results stay **bypassable and out of the scoreboard** (biRISC-V), so a
dependent ALU chain never stalls; only loads, divmul, and serializing ops
carry a pending bit. Stores carry none — they produce no register result.

## The structural invariant

The correctness of the whole scheme rests on one assertion-by-construction,
which P0.1 verifies: **no combinational arc from a cache `busy`/hit input to
any issue-enable or MMU output**. `busy`/hit feed only the completion FSM,
whose output is the registered `mem_pending_q`. The probe confirms the
longest path into the issue enable originates at a flop, and a Verilator
assertion guards the wiring.

## What P0.1 proves

P0.1 wires the issue gate (scoreboard read → `can_issue` → ID/EX enable)
with realistic fanout (all source operands, all enabled flops) and the
registered load-completion path, and confirms two things:

1. **Timing** — the `mem_pending_q`/scoreboard → `can_issue` → enable cone
   closes at 50 MHz with the probe→full margin.
2. **Structure** — there is no combinational `busy → issue`/`busy → MMU`
   arc; the gen2 floor cone cannot form.

A pass is the deletion of gen2's floor — the change the whole gen3 redo is
organized around.

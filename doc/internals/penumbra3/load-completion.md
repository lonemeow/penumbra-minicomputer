# Penumbra/3 — Load completion & back-end stall

> **Applies to:** Penumbra/3 · pipelined core (in design).

This is the single highest-value change in the gen3 plan: the back-end
stall that gates issue on a **registered** load-completion bit instead of
the live cache `busy`, deleting the cross-die combinational cone that was
gen2's fmax floor. It specifies the load-completion model (hold-and-
complete), the issue interlock, and the structural invariant
[Phase-0 probe P0.1](./phase0-probes.md) checks.

It builds on the [timing outline](./timing-outline.md) (the back-end stall
section), the [MEM1/MEM2 stage](./mem-stage.md) (which produces the verdict
this consumes), and the [bus master](./overview.md#fork-boundary) (which the
fill is launched through). The full forwarding network is a separate spec;
this doc covers only the load path and the issue gate it feeds.

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
  (MEM2→EX bypass covers the normal distance), it stalls at issue — reading
  a flop, never the cache verdict.
- The bit clears on completion (the cycle the hit resolves, registered).

Load-use distance is ~1–2 cycles with the MEM2→EX bypass; the exact figure
is pinned in the pipeline-stages spec.

## Load miss — hold-and-complete

A miss does **not** re-run MEM1 each cycle. It is *held* and *completed*:

```mermaid
graph LR
    M2[MEM2 miss] --> HB[one-entry<br/>hold buffer]
    HB --> LP[load_pending_q<br/>set]
    HB --> FILL[fill launched once<br/>registered txn]
    FILL --> BUS[penumbra3_bus_master<br/>line fill]
    BUS --> CMP[registered completion<br/>{data | fault}]
    CMP --> WB[held load -> WB,<br/>load_pending_q clear]
```

- **One-entry hold buffer** parks the missing load (single-outstanding ⇒ one
  entry). MEM1 is not replayed.
- **`load_pending_q`** (a flop) is the pipeline gate. The pipe holds via
  back-pressure off *that flop*, not off the live `busy` — this is the only
  change vs gen2's load path. **Hold-the-pipe is the design point, not a
  placeholder.** Issuing independent work past a miss (non-blocking loads)
  needs multi-outstanding tracking and a completion-driven wakeup of waiting
  dependents — a match/CAM landing squarely on the issue cone this design
  works to keep shallow. It trades the fmax gen3 exists to protect for IPC
  only on miss-heavy code, and going multi-outstanding would also cost the
  precise-fault property below. It is out of scope by design, not deferred.
- **The fill is launched once** as a registered line transaction through the
  [bus master](./overview.md#fork-boundary) (validated in P0.4); the fill
  FSM holds the request.
- **Completion is a registered event** delivering `{data}` or `{fault}` to
  the held load, which then advances to WB and clears `load_pending_q`.

## What the pipe gate freezes

`load_pending` is back-pressure with a boundary, not a global freeze: it holds
the launch side and lets the commit side drain.

- **Held while a load is pending:** MEM1, the MEM1/MEM2 register, the
  [address-translation launch](./mem-stage.md), and issue (a dependent stalls
  on the scoreboard, see [the issue gate](#the-issue-gate)). The missing load
  and every younger instruction hold in place.
- **Not held:** the MEM2/WB register. It carries instructions *older* than the
  held load to the commit point, and an older instruction must retire while the
  load waits. A pending load drives that register to bubbles — its MEM2 verdict
  does not commit — and the held load itself reaches a register through the
  completion path above, never through MEM2/WB, so draining the register is
  always correct.

The MEM2/WB register's only back-pressure is a genuine downstream stall: the
commit stage holding for a multi-cycle writeback (the divmul dual-register
retire, which writes its two destinations through the single regfile port over
two cycles). Folding `load_pending` into the MEM2/WB freeze re-presents that
already-committed writeback as a fresh slot and retires it twice — the two
freeze sources stay separate.

## Precise faults for free

The held load is, by the in-order single-outstanding invariant, **always the
oldest in-flight access** when its result lands. So whatever the completion
carries — data or fault — arrives at a precise point: nothing younger has
committed, and a fault is squash-younger + redirect, identical to an
alignment/TLB fault taken at MEM2. The out-of-order machine works for this;
in-order single-outstanding gets it by construction. A faulting fill
installs nothing (the line stays invalid); the fault rides the completion,
transient, not cached.

## Uncached and single-beat — the same path

An uncached or sub-word access is a single-beat registered transaction (the
bus master's non-line path): the held, oldest access waits for
`{data | fault}`. No line, no caching — just transactional delivery. **Boot
device-probing depends on this**: the probing load is the oldest, held, and
a bus fault from a missing/erroring device is delivered to it precisely, so
the probe handler runs.

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
carry a pending bit.

## The structural invariant

The correctness of the whole scheme rests on one assertion-by-construction,
which P0.1 verifies: **no combinational arc from a cache `busy`/hit input to
any issue-enable or MMU output**. `busy`/hit feed only the completion FSM,
whose output is the registered `load_pending_q`. The probe confirms the
longest path into the issue enable originates at a flop, and a Verilator
assertion guards the wiring.

## What P0.1 proves

P0.1 wires the issue gate (scoreboard read → `can_issue` → ID/EX enable)
with realistic fanout (all source operands, all enabled flops) and the
registered load-completion path, and confirms two things:

1. **Timing** — the `load_pending_q`/scoreboard → `can_issue` → enable cone
   closes at 50 MHz with the probe→full margin.
2. **Structure** — there is no combinational `busy → issue`/`busy → MMU`
   arc; the gen2 floor cone cannot form.

A pass is the deletion of gen2's floor — the change the whole gen3 redo is
organized around.

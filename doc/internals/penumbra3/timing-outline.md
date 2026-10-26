# Penumbra/3 — timing-critical design rules

> **Applies to:** Penumbra/3 · timing-critical design rationale.

The structural rules gen3 must follow to close a *reliable* 37.5 MHz on the −6
ULX3S (ECP5) with headroom for its staged features, distilled from the gen2
timing-closure work. Each point is a rule + why + the gen2 mistake it avoids.

The per-cone budgets below are 50 MHz ones (a 20 ns period). A bare cone reads
well above what the same logic makes on the full top — gen2 measured ~55 MHz
against ~31 — so a cone designed to 50 is what closes 37.5 on the full top.

This is the **rationale layer**: the [gen3 plan](./overview.md) builds on
these rules (and references them rather than restating them), the
[Phase-0 probes](./phase0-probes.md) test the load-bearing ones, and the
detailed per-subsystem design docs (design-decisions, pipeline-stages,
memory-interface) follow.

The gen2 diagnosis these rules react to is recorded in the
[gen2 abandonment banner](../penumbra2/overview.md) and the fmax-closure
round in [the gen2.5 TODO entries](../../TODO/hw-penumbra2.md). The rules are grounded in a
survey of open in-order / pipelined softcores — biriscv, cva6, cvw (Wally),
mor1kx, vexriscv.

---

## Rule Zero — the only rule that actually matters

**On a −6 ECP5, a combinational signal between two blocks the placer may separate
is a ~20 ns routing path = a timing failure.** Gen2 fails everywhere because it is
combinationally coupled everywhere; the limiter "teleports" between subsystems
because they are all one combinational web. Therefore:

- **Every inter-block boundary is a registered handshake.** No exceptions.
- **Every *fast* boundary is a *decoupled* req/rsp handshake** (separate request and
  response streams), so registering it adds *latency*, never *per-transfer bubbles*.
- The **~5.8 ns NOREG BRAM Tco is a hardware floor** (−6 grade). Design every BRAM
  read so it lands on a flop with *nothing else* combinationally gating the pipe;
  ~14 ns of logic budget remains in a 20 ns period.

Corollary on register placement:
- **Transaction-boundary register** (flop the request/verdict at a unit boundary):
  +1 per *transaction*, 0 per word, 0 per hit. **Always use this.**
- **Per-beat handshake-loop register** (flop inside a streaming busy/ack loop): +1
  per *beat*. **Never.**

---

## 1. Front-end — mostly proven in gen2.5, carry it forward

- Fetch PC is **always a registered value** (`pcReg`), never in the stall cone.
- Only a **1-bit `ready`** crosses backward into fetch — gates clock-enables, never
  the PC mux or RAM address/enable.
- **Fetch buffer (FIFO)** decouples front-end from back-end.
- **All prediction is fetch-domain and BRAM-backed, indexed not associative:**
  a bimodal direction table read beside IF1 (gshare measured and does not pay),
  training registered off the fetch loop. **Direct targets and the RAS via an
  IF2 pre-decode** (detect the transfer from the fetched word) — the RAS a plain
  ring with no empty check — *not* BTB entries (capacity), *not* ID-stage (a
  decode-stage predictor is marginal and registers to nothing)
  ([measured](./branch-prediction.md)). The IF2 redirect closes in its cycle
  (P0.5), so a target buffer is only an option for indirect targets.
- **IF2 works per way, then selects.** The word→bundle decode and the pre-decode
  run on each way's word straight off the block RAM, beside the TLB match and the
  tag compare; the hit selects finished results. The predicted redirect waits on
  the hit alone — never the translation fault or cacheability, which a trap or a
  replay settles from registered state — and the RAS updates a cycle after its
  transfer enqueues.
- Mispredict = **registered flush + redirect**, never enters the stall OR-tree.

## 2. Back-end stall — the load-use freeze (THE critical one)

- gen2's worst disease: dcache **combinational** `busy` → spine stall → ID issue
  *and* MMU verdict, one die-spanning cone. Several ~26 ns paths are this *same*
  cone with different tails (load-use, MMU). Fixing it collapses a whole cluster.
- **No combinational path from cache hit/busy into issue or the MMU.** The fix is a
  **registered `mem_pending` flop**, not the combinational `~hit`. Dependents stall
  at *issue* reading that flop (scoreboard bit); the `busy → ID` cone is gone.
- Hazard interlock is **local FU-busy/ready bits** (scoreboard-style), not a global
  spine OR-tree that spans the pipeline. (biRISC-V/CVA6 have no "spine"; the
  scoreboard *is* the stall logic.)

### Load-miss completion model (the "replay" — pinned down)

"Replay" here is **hold-and-complete, NOT re-run MEM1 per cycle.** The mechanism:

- On a MEM2 miss, park the load in a **one-entry load-hold buffer** (single-
  outstanding ⇒ one entry). MEM1 is **not** re-run; the fill is **launched once** as
  a registered transaction (the fill FSM holds the request, as gen2 already does).
- The pipeline gate is the **registered `mem_pending`** bit, not the live cache
  busy. The load is **held, not retired**, until the transaction completes.
- Completion is a **registered** event delivering `{data}` or `{fault}` to the held
  load → it advances to WB.

**Faults are precise *for free*, by the in-order + single-outstanding invariant:**
the access waiting on the bus is **always the oldest in-flight**, held in the
buffer, when the result lands — so whatever comes back (data or fault) arrives at a
precise point, nothing younger has committed, and a fault = squash-younger + redirect,
identical to an alignment/TLB fault at MEM2. This is the property an out-of-order
machine has to *work* for; in-order single-outstanding gets it by construction.

- **Bus fault on a fill → no per-line "faulted" bit.** A faulted fill **installs
  nothing** (line stays `valid=0`), and the fault rides the **completion** to the
  held load (transient, not cached). **Reuse gen2 verbatim:** `o_fill_fault` —
  *"a faulting beat aborts the line: serve a fault and install nothing."*
- **Uncached accesses use the same path** — a single-beat registered transaction;
  the held (oldest) load waits for `{data | fault}`. No line, no caching, just
  transactional fault delivery. **Boot device-probing depends on this** and works:
  the probing load is the oldest, held, and a bus fault from a missing/erroring
  device is delivered to it precisely → the probe handler runs. (gen2's `S_BEAT` +
  `o_fault`-on-busy-drop is already this — carry it forward unchanged.)

**The only gen3 change** vs gen2's load path: move the pipeline gate from the
combinational cache `busy` to a registered `mem_pending` flop, and park the load in
a hold buffer instead of freezing the whole pipe combinationally. The fill FSM,
install-nothing-on-fault, serve-fault-on-completion, and single-beat/uncached paths
are all reused as-is.

### Registered decode→issue boundary (the forwarding-cone fix)

A gen2.5 seed that *barely* met 37.5 had its critical path **here**, not in any
cache/MMU cone: `fetch-buffer FIFO read → decode (penumbra2_decode, incl. a
carry-chain op-decode) → regmap (phys_src/phys_dst) → scoreboard/forward interlock
(src_reads → load_use_stall) → can_issue → issue → ID/EX flop enables`, ~26 ns, with
the issue/enable term ping-ponging ID→spine→ID on the tail. nextpnr labels the middle
with `fwd_a_en` nets so it *reads* as a "forwarding path" — but the forward-control
bits are not the cost. The cost is that **decode, register-map, the hazard/forward
compare, and the issue decision are one combinational cone off the raw FIFO word.**

- **Rule: the fetch buffer presents a *decoded bundle*, never a raw instruction word
  — and ID stays the single decode+issue stage; only the heavy combinational decode
  relocates.** Decode-and-issue in one stage is normal and fast (it is what a classic
  RISC ID does) *when decode is cheap*: RISC-V's fixed rs1/rs2/rd fields make it a
  shallow SOP, the one thing the survey says does NOT transfer to a custom ISA
  (VexRiscv). Penumbra's word→bundle decode is heavier (the CCU2 in
  `d_alu_op`), so it cannot share a cone with regmap+forward+issue+regfile-read.
  - **Default — pre-decode on FIFO *enqueue* (no extra stage):** decode the word into
    the control bundle on the IF2→push (fetch domain has slack), store the bundle, ID
    reads a registered pre-decoded slot. ID's cone now *starts* from registered fields.
    Zero extra latency — no change to mispredict penalty or load-use distance; decode
    rides the FIFO cycle already paid. Wider FIFO (store bundle, drop the raw word).
  - **Write the decode parallel-then-select, not select-then-decode.** gen2.5 saw ID
    become a cone because the four instruction formats decoded serially — resolve the
    format, *then* decode by it. Decode all four formats off the raw word in parallel
    and mux the result on the format bits, so the only serial step is the final select.
    The format bits sit at the top of the word, so detection can move to the front
    (IF2 / enqueue). Applies wherever decode runs; on the enqueue side it has the slack.
  - **Keep regmap *in* ID** — do NOT pre-map at enqueue. It is a shallow index mux and
    it depends on live `i_supervisor` (USP/SPR banking); pre-mapping at fetch would use
    a stale mode if a mode-change is in flight ahead of the slot. Only the
    mode-independent word→bundle decode moves.
  - **Fallback — a real DEC→ISSUE split** (biRISC-V High-FMAX) *only if* the residual
    cone (regmap → regfile-read → forward-mux → issue) is still too long after
    pre-decode. Costs a stage (+1 mispredict / load-use cycle), so measure first.
- **This is NOT a forwarding floor.** In *every* surveyed core the forward-control
  compare is reg#-equality on **already-registered, already-decoded** fields — Wally
  `ForwardAE/BE` (Rs1E/Rs2E vs RdM/RdW), mor1kx `execute_rfd_adr==decode_rfa_adr`,
  biRISC-V/CVA6 scoreboard read. 1–2 LUT levels, ~2–3 ns, never a 25 ns path.
  VexRiscv closes **50 MHz on this fabric** with full bypass. Once decode is
  registered, gen2.5's `src_reads`/`load_use_stall` compare *is* that shape.
- **Tail fix (Rule Zero):** the `issue`/enable term must be **local** to the block
  that owns the ID/EX flops — don't route it ID→spine→ID. The seed crosses that
  boundary twice on the enable cone.
- **Keep the gen2.5 wins:** ALU results stay bypassable / out of the scoreboard
  (biRISC-V) so dependent ALU chains never stall; interlock-vs-bypass is an explicit
  Fmax lever (VexRiscv: interlock 51 vs bypass 45 MHz on iCE40) if a late bypass mux
  ever caps a corner.

### Multi-cycle and serializing instructions (div/mul, WRSYS, ERET)

All three reduce to **two registered primitives already in the design** — the
registered *completion* (the §2 load model) and the registered *serialize+redirect*
(the mispredict path). Neither adds combinational depth. Rule: **rare + slow ops pay
cycles, never critical-path logic.**

- **div/mul — long EX latency + dual-register WB = the load-completion pattern.**
  - Launch into a side FU; mark a **registered pending** bit (scoreboard dst + aux —
    gen2.5 already carries `o_phys_dst_aux`/`i_aux_dst`). Dependents stall at issue on
    the scoreboard bit, never on a combinational EX-busy.
  - Result returns on a **registered completion** delivering `{dst, aux}` or a fault
    (div-by-zero) — same event shape as a load completion; precise by in-order-oldest.
  - **Dual WB = serialize two single-port writes** (low then high). Rare + already
    multi-cycle ⇒ +1 WB cycle is free; avoids a 2-write regfile (doubled LUTRAM ports).
    Aux scoreboard entry holds until the second write retires.
  - **Keep aux forwarding deferred** (gen2.5 `divmul_aux_stall`): stall the dependent,
    don't build a combinational forward from the div completion into EX (long path,
    rare case — same reasoning as load-use taking a bubble).
  - Policy: **hold-the-pipe (Wally) is the default** — simplest, timing-trivial, IPC
    cost only on div-dense code. Scoreboard-overlap (independent younger pass,
    biRISC-V) is a later IPC lever, *only if* it keeps in-order retire (else imprecise).

- **WRSYS / ERET — serialize + redirect at commit, via the in-order invariant.**
  - **"Older must drain" is free — don't compute it.** At WB the instruction IS the
    oldest; a cross-pipe "anything older in flight?" scan *is* the Wally global cone.
    The serialize condition is local + registered: *I am at the commit point.*
  - At commit: write new state **registered** (SR/supervisor, PT-base, ASID, cache-en),
    then assert a **registered flush of younger + redirect** (ERET→EPC, WRSYS→next_pc)
    — reuse the mispredict redirect path verbatim.
  - **Multi-cycle side effects** (cache/TLB flush) hold via a **registered post-commit
    busy** (`o_post_commit_wait`) until the side-effect FSM signals done — a registered
    gate, not a combinational drain cone.
  - **Do NOT classify WRSYS by register identity in the core** — a layering smell
    (WRSYS is an opaque uncore write; the core shouldn't know which sys_reg matters).
    **Default: serialize *every* WRSYS** — zero classification, trivially correct, and
    the form to build first. **It is not noise, though: the TLB refill is a hot
    WRSYS path.** Measured on gen2.5, a WRSYS costs ~6.3 cycles, the kernel's refill
    handler issues six per refill (~28 % of a ~137-cycle refill), and refills run
    4–6 per 1000 instructions on Doom and the kernel paths — several percent of all
    cycles, more at gen3's lower base CPI
    ([memory-performance](./memory-performance.md#tlb-sets-and-a-cheaper-refill)).
    A larger TLB shrinks the refill count, and so the case: 256×2 removes 92–98 % of
    refills, so selectivity is re-measured once the TLB has grown. Any selective form
    keeps what software sees: every WRSYS is context-synchronizing
    ([sysregs](../../system/sysregs.md)), and existing kernels rely on it. Move the
    policy **out** of the core, not into the decoder:
      - **uncore-reported serialize bit** on the WRSYS response: the device sets it on
        writes whose effect a younger, already-fetched instruction could observe — for
        the MMU, a TLB install, not the staging of its VPN. Core stays dumb, no ISA
        change, existing kernels unaffected. Preferred. *(device declares)*
      - **explicit barrier op** (SFENCE/FENCE.I style, cf. Wally `CSRWriteFenceM`):
        only as an opt-in path a kernel built for it uses, fencing once per PTE
        *batch* — never by making plain WRSYS non-serializing, which every existing
        kernel would break on. *(software declares the sync)*
  - Keep the **WRSYS value interlock** (gen2.5 `wrsys_value_stall`): the datum rides
    registered `idex_op_b`, so stall a reader until its writer commits — else a TLB-miss
    handler installs a stale PTE and re-faults forever. Scoreboard stall ⇒ timing-safe.

- **This flush is what makes pre-decode safe** (closes the §2 decode boundary).
  ERET/WRSYS flush the front-end *including the pre-decode FIFO*, so no pre-decoded
  bundle is ever stale under the new mode; regmap stays in ID on live `i_supervisor`,
  always mode-correct.

## 3. Memory stage — split MEM1 / MEM2

- gen2's single-MEM + 1-cycle stall costs **~0.35 CPI** (every load/store) and halves
  memory throughput. The BRAM cache is a 2-cycle launch/resolve; *pipeline* it.
- **MEM1:** EA (registered out of EX) → launch cache index + **sync TLB BRAM read**.
- **MEM2:** cache data + tag compare + **TLB verdict** resolve.
- This pipelines loads (no per-op bubble) **and** provides the cycle the sync-BRAM
  TLB needs (see §5). One restructure, both wins.

## 4. Caches

- **Hit path:** `EBR read → tag compare → way mux → flop`, nothing else. The hit is
  a **registered qualifier consumed by replay**, not a combinational stall, and
  never a wide enable: a register whose load would depend on it (MEM2/WB)
  loads unconditionally and lets its valid bit carry the decision.
- **Bookkeeping off the hit→busy cone:** PLRU touch, dirty, write-hit are
  registered/deferred (gen2's L1 already does this — keep it).
- **A store hit is written a cycle after it resolves,** from registers, so
  the hit reaches a register's D and never a block RAM's write enable. The
  cache merges the store's bytes over the word its own lookup read and
  writes the whole word. A lookup that resolves before the block RAM shows
  that word, in the write's cycle or the next, takes it from the write
  register or a copy one cycle behind it: a mux on each way's word,
  selected by a compare of registers, which the tag compare outlasts.
- **A held lookup is read again every cycle.** The L1 keeps no copy of a
  lookup: with nothing launching it reads the last lookup's set again, so
  a verdict its consumer holds follows a fill or an invalidate with no
  strobe to ask for it, and the store bypass never outlives the write's
  two cycles. A line shows to a held lookup two cycles after its install,
  and the L1 reports an install into the held lookup's set in the
  install's cycle, rather than a bypass putting a mux between the tag
  block RAM and the compare.
- **L1: write-through**, keep the timing-critical level simple.
- **Size the L1 per board, and read size and ways together.** A VIPT L1 indexes
  within the 4 KB page, so a way holds at most 4 KB: 8 KB 2-way keeps the compares
  of 4 KB 2-way, and anything above 8 KB needs at least 4 ways, each one more
  physical-tag compare fed by the TLB verdict in the same cycle (IF2 and MEM2). At
  the line-wide fill's cost, 8 KB 2-way cuts L1 fill stall 19–21 % against 4 KB and
  16 KB 4-way 35–49 %, most of the step past 8 KB on the fetch side
  ([memory-performance](./memory-performance.md#l1-geometry)). The ECP5 takes 8 KB
  2-way; an Artix-7 board has the block RAM and timing room for 16 KB, and the two
  sides need not match: IF2 can take 4 ways while MEM2 keeps 2.
- **State BRAM read/write exclusivity in the RTL.** Where a BRAM's read and write
  could coincide, yosys adds a bypass that returns the old data — flops capturing
  the write and a mux on the EBR output, i.e. logic after the Tco that the
  hit-path rule forbids. gen2's L1 tag array carries 27 LUTs and 51 flops of it;
  `penumbra3_tlb_store` about 134 LUTs and 137 flops. yosys drops it only when the
  read and write *enables* exclude each other — the write gated off while a read
  launches, or the read enable gated by the write strobe — never through an
  address compare. A read suppressed only when it meets the write's address (a
  store hit written a cycle after it resolves) is exclusive but unprovable:
  mark that memory `no_rw_check` and assert, beside it and on the same enables
  and addresses, that no entry is read and written in one cycle. Only where the
  memory's own read enable carries the exclusion: an invariant another module
  keeps (no launch during a fill) is written into the enables instead, since
  the attribute would assume it unchecked.
- **L2: write-through, posted; write-back / write-allocate only if stores prove
  throughput-bound.** L2 takes a store-buffer entry's write in its pipeline and
  serves hits while its bus port carries the write-through, so a fill L2 serves
  never waits for the bus
  ([memory-performance](./memory-performance.md#store-buffer-at-the-core-boundary)).
  Store latency, the measured binding cost, is a store-buffer problem
  ([memory-performance](./memory-performance.md)). If write-back lands, its
  complexity (dirty + eviction-writeback FSM) lives in the L2 **miss path** — off
  the hit path and off critical routing.
- **Do not** go direct-mapped for timing — it wrecks IPC (conflict misses) and
  barely shortens the path (the way-mux is ~1 LUT level).

## 5. MMU / TLB

- **Unified TLB architecturally** (one entry set, one miss handler, one SPR
  interface) — keep the kernel sane. *Not* mor1kx's software-visible split (real
  kernel misery; confirmed on the or1k port).
- **Duplicated BRAM storage:** I-copy + D-copy, hardware-coherent (refill/invalidate
  fan to both; reads independent). Each copy = `1 read + 1 write/sysreg = 2 ports` =
  fits a DP16KD. Sync registered read. Cost is 2× tiny storage; you have the EBRs.
- **Launch the TLB read in MEM1, verdict in MEM2.** Hit-detect runs against the
  *registered* BRAM output → short cone, not gen2's ~8.7 ns async-LUTRAM
  read-plus-compare.
- **Fault contract is commit-time** (FADDR/FSTAT latch at WB, not on the translate
  path) — gen2 already does this (Decision 16). Re-pipelining the translate is
  **kernel-invisible**. Carry it forward.
- Keep TLB associativity modest; duplication + sync read makes 2-way fit.
- **Grow the TLB by sets, not ways.** In BRAM, depth is already paid for: 256×2
  costs no EBRs and no compare width over 32×2, and removes 92–98 % of refills on
  Doom and the kernel paths, where refills are the largest single cost measured
  ([memory-performance](./memory-performance.md#tlb-sets-and-a-cheaper-refill)).
  Beyond 32×2 the software interface needs `MMU_CONFIG` version 2 and a wider
  `TLB_INDEX` ([`mmu.md`](../../system/mmu.md)) — doc first.
- **Invalidate in the MMU, off the lookup path.** Software invalidates by searching
  slots: `tlb_invalidate_addr` reads a set's ways through `RDSYS`, and
  `tlb_invalidate_asid` — run on every exit and on every exec that reuses its address
  space — reads and rewrites every slot. The sweep grows with the TLB: at gen2.5's
  per-op costs (`WRSYS` ~6.3 cycles, `RDSYS` ~2) it is ~1 k cycles at 32×2 and
  ~5–8 k at 256×2 (estimated, not measured), and `RDSYS` itself stops being a fast
  op once a TLB read is a registered BRAM read. Version 2 is the place for two
  sysreg ops that move the search into the MMU:
  - **invalidate by address** — `WRSYS` of VA|ASID; an FSM reads the set through
    each copy's sysreg/write port, compares the valid ways against the written
    VPN/ASID/G in a registered stage, and writes V=0 into the matches in both copies
    (~3 cycles). Matching only valid ways also retires a software hazard: an
    invalidating `TLB_PTE` write commits whatever `TLB_VPN` was last staged, so a
    dead slot's VPN word can name a live entry in the other way;
  - **flush all / flush by ASID** — the same FSM stepping through the sets, about one
    cycle per set to flush all, two (read, then write) to flush by ASID.

  Both are multi-cycle side effects behind the registered post-commit busy
  ([multi-cycle and serializing
  instructions](#multi-cycle-and-serializing-instructions-divmul-wrsys-eret)): the
  `WRSYS` completes when the FSM is done — no poll, no new handshake. The
  translation read and hit compare never see the FSM; the only new sharing is one
  more driver on the second port's address and data mux, which already carries refill
  writes and sysreg reads. A per-line cache invalidate on BRAM tags (`INVAL_LINE`,
  reserved) is the same read-compare-clear FSM behind the same wait. Without the ASID
  flush, the kernel can avoid the sweep instead by giving a torn-down pmap's address
  space a fresh ASID: stale references still fault, since no entry carries a live
  ASID until the rollover flush, at the cost of one ASID per exec.

## 6. Bus / uncore — the Amiga decouple

- **RAM stays reachable on the bus.** A RAM board in an autoconfig slot keeps
  working, in the bus's sync and async forms — boards without on-board SDRAM need
  it, and it is core to the design (Amiga Zorro model). That rules out taking
  memory *off* the bus, not a faster path beside it: a native port from the uncore
  to on-board RAM, with the bus still serving peripherals and add-on RAM, stays an
  option (the PC northbridge; the A3000's motherboard Fast RAM beside Zorro III).
  On the profiled workloads it adds little over write combining on the bus
  ([memory-performance](./memory-performance.md#sdram-side-write-combining)).
- The fix is **decoupling the CPU/L2 from the bus, Amiga 68040↔Zorro style:**
  registered **line-transaction** request out, streamed response back through a fill
  buffer, **cache hides the latency**. The CPU clock is independent of bus speed.
- **No combinational path through the bus** (gen2's device-busy aggregate reaching
  the L2 PLRU is the disease).
- **Fast path** (cacheable line fill): decoupled req/rsp, registered, back-to-back
  streaming preserved (latency +K, throughput untouched).
- **Slow path** (async 4-phase peripherals): registered *coupled* handshake, +1 per
  transfer (fine — slow, rare). The 4-phase protocol is **preserved** for
  peripherals (discrete-logic goal intact); only the CPU↔bus boundary is registered.

## 7. L2 fill — burst, not per-word

- gen2 fills per-word, so L2 hit latency (and any registered L2 verdict) costs **per
  word**. Burst (**hit-once, stream-words**) makes it **per line**.
- The fill burst is **read-only cacheable** — no uncached/sub-word/RMW hazard, so it
  pipelines safely *independent of* the single-beat (uncached/sub-word/write) path,
  which keeps its single-outstanding handshake.
- **L2 produces registered outputs** (busy/data/fill_done) so the L2 tag cone never
  reaches the L1/arbiter combinationally.
- **Prefer line-wide to word-streamed.** One L2 access returns the line (128-bit
  rows, the same EBR count as word rows) and L1 installs it in one cycle (a
  36-bit-write / 9-bit-read array, word select inside the EBR). On an L2 miss, hand
  L1 the line the bus just delivered instead of re-reading it (fill-through).
  Estimated on gen2's structure: L2-hit fill 16 → ~5 cycles, L2-miss fill
  37 → ~24 ([memory-performance](./memory-performance.md#the-line-wide-transfer)).
- **Budget the fill path's hops.** Each registered hand-off on it costs one cycle
  per fill, and fills run at 40–95 per 1000 instructions with a 4 KB L1. About 19
  cycles of an L2 miss belong to the bus, the clock crossings and the DRAM; aim for
  ~5 around them.

## 8. FPGA / tooling / hardware reality

- **`keep_hierarchy` helps placement** in this flow (confirmed) — keep it.
- **−6 grade is the floor** (5.8 ns BRAM Tco). 50 MHz cones are reachable (VexRiscv
  proves it on comparable fabric) but tight. **Design for margin, not
  barely-closing** — a 1-of-N-seeds close is *not* a close.
- CPU clock ∈ {25, 37.5, 50} from the 600 MHz VCO (SDRAM phase fixed). Target
  **reliable 37.5** (600/16) across a seed sweep of the full top; 50 (600/12) is not
  a goal, and the cones are designed to it so that 37.5 closes with headroom. The
  VCO is pinned: the phase detector needs ≥10 MHz (5 MHz never locked) and USB's
  exact 60 MHz shares the PLL, which leaves 600 MHz as the only legal VCO.
- **No synchronous crossings between PLL clocks.** nextpnr reports a path between
  two PLL outputs as a delay only — no slack, no failure, no placement weight — so
  a crossing that relies on a fixed clock ratio is unverified by the flow. Cross
  with the async FIFO, which is correct whatever the placer does
  ([memory-performance](./memory-performance.md#synchronous-cpusdram-clocking--ruled-out)).

## 9. Reuse boundary

- **Keep as-is:** SoC peripherals (UART, SPI/SD, autoconfig, video), the async
  Penumbra Bus *for peripherals*, the LLVM backend, the NetBSD port (kernel mostly
  intact — unified-TLB + commit-time-fault contracts preserved), the ISS/tests, the
  ISA.
- **Rewrite (new files; can't touch shared gen1/gen2 RTL):** the core — pipeline,
  stall/replay control, caches, MMU storage, L2, fill sequencer, the CPU↔bus
  decouple.

## What gen2 already got right — don't relearn these

- Front-end decoupling (registered `pcReg`, fetch buffer, the C1–C4 fixes).
- MMU registered verdict + **commit-time fault registers** (kernel-decoupled).
- Cache **PLRU / dirty / write-hit deferred** off the hit→busy path.
- **Unified software TLB** (the right kernel-sanity call — preserve, just duplicate
  the storage).
- The disciplined `txn_arbiter` **registered launch register** (+1/transaction,
  0/word, 0/hit) — that's the *template* for every boundary in gen3.

---

### The four structural changes that matter most (everything else is hygiene)

1. **Registered load-completion back-end** (hold-buffer + `mem_pending` flop, §2) —
   kills the load-use freeze *and* the MMU verdict path (same cone). The single
   highest-value change. Faults stay precise by the in-order-oldest invariant; the
   cache's fault machinery is reused as-is.
2. **Registered decode→issue boundary** (§2, "the forwarding-cone fix") — the cone
   that capped a *barely-37.5* seed. Decode off the raw FIFO word, registered; the
   forward/hazard compare runs on registered reg#s (Wally/mor1kx/VexRiscv shape).
   Not a forwarding floor — a missing flop. Pre-decode-on-enqueue is the cheap form.
3. **CPU↔bus transaction decouple** (Amiga model) — kills the device-busy mesh,
   keeps memory on the autoconfig bus.
4. **Split MEM1/MEM2 + duplicated BRAM TLB + L2 burst** — pipelines memory, shortens
   the MMU cone, cuts SDRAM write traffic, all interlocking.

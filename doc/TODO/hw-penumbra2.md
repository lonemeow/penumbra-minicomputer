# Hardware: Penumbra/2 and 2.5

Entries specific to the gen2 / gen2.5 microarchitecture.

## Hardware: Penumbra/2 retire pulse counts faulting/trapping slots

BREAK is now a real EX trap (along with SYSCALL): it sets `fault_pending` in
EX, commits at WB, and vectors to `VEC_BREAK` through the same entry path as
every other synchronous exception — there is no hardware halt. The testbench
still detects program end by watching `o_retire_valid` + `o_retire_op_class`
for `OPC_BREAK`, which works because a trapping BREAK still leaves WB as a
valid slot the cycle it takes its trap.

Residual: `o_retire_valid` is currently `memwb_valid`, so it pulses for
faulting and trapping slots too (the alignment-fault load, the BREAK/SYSCALL
trap). As an insns-retired perfctr source that over-counts — a faulting
instruction is re-executed after the handler, not retired. When a gen2 perfctr
lands, gate the retired count by `~fault_pending`.

The program-end testbench keys on `o_retire_op_class == OPC_BREAK`, and that
*was* spuriously trippable: an IF-faulted slot carries a garbage instruction
word whose decode can be `OPC_BREAK`. A fetch bus fault whose stale read
happened to decode as BREAK ended the run before the handler dispatched (found
via `isa/test_bus_fault_fetch`). Fixed: `penumbra2_id_stage` neutralises a
faulting slot's `o_op_class` to `OPC_ALU` (the same reason `o_is_trap` is gated
there), so an inert slot never reports a real op class at retire. The remaining
`o_retire_valid` over-count is the perfctr-only item above.

## Hardware: Penumbra/2 spine testbench lacks cross-stage stall cases

A taken `BL` pinned in EX by a stalling MEM lost its link write
(fixed in `e3d8883eda93`).  Leaf testbenches drive their own stalls,
but `tb_penumbra2_spine` drove `i_dmem_busy = 0` always, so a stall
interacting across stage boundaries — the only place that bug shows —
had no coverage.  The `run_mem`/`tick_mem` model (a configurable
multi-cycle MEM latency) and Test 5 now cover the `BL` case.  Still
worth adding on the same harness:

- drain-commit under a MEM stall;
- a RAW consumer waiting on a producer stalled in MEM;
- a fault committing while a younger branch is resolved under stall,
  which checks that `wb_fault_commit` still kills regardless of
  back-pressure (the opposite of the `BL` fix);
- a divmul immediately followed by a branch.

The per-cycle EX/MEM/WB occupancy window in `tb_penumbra2_interactive`
(`+pipe_lo=`/`+pipe_hi=`) is what exposed the bug: a retire-only trace
cannot show a resolve-then-squash.

## Hardware: gen2.5 feature status and measurements

### gen2.5 priority is ranked from the stall profile — real workload first

The build order below is **data-driven from the gen2 stall profile**,
re-measured on hardware after the perfctr stall-attribution fix (the
carried-cause counters, `a52ab8c1d1f7`). The pre-fix profile
that originally set this ranking was read off the mislabeled residual: it
reported `flush` as the dominant stall (~30% on Dhrystone, ~35% on NetBSD) and
put branch prediction first. That was an artifact — back-end latency tails
leaking into `flush` — and the corrected profile re-ranks the levers.

- **Dhrystone on gen2 (HW, 25 MHz):** CPI 3.13. Stalls (% of cycles):
  hazard 28.6, store 18.3, flush 11.6, load 6.0, funit 3.3, ifetch ~0.
  L1I/L1D ~99.9% hit, L2 nearly idle.
- **Dhrystone on gen2.5 (HW, 25 MHz):** CPI 3.05. hazard 29.4, store 18.8,
  flush 9.2, load 6.1, funit 3.4, ifetch ~0 — *same back end*; `flush` falls
  ~2.4pp from the predictors (and ~1.7M fewer wrong-path I-fetches), which is the
  entire gen2→gen2.5 gain (DMIPS 7.11 → 7.29, +2.5%).
- **NetBSD pbench profile (HW, 25 MHz, corrected counters) — the real-workload
  re-measurement, now in.** Kernel-path stall % of cycles, gen2.5 core at the
  BTFN+RAS milestone (both predictors later removed in
  [the fmax-closure round](#gen25-status-fmax-closure-round-375-mhz); the
  surviving predictor is the fetch-time BTB):

  | bench         | CPI  | hazard | ifetch | store | flush | load | funit |
  |---------------|------|--------|--------|-------|-------|------|-------|
  | getpid        | 4.12 |  22.2  |   8.1  |  22.5 |  13.1 |  7.6 |  2.1  |
  | clock_gettime | 4.65 |  18.8  |  16.0  |  20.2 |  11.7 |  5.6 |  6.2  |
  | pipe_pingpong | 4.63 |  22.1  |  19.3  |  12.1 |  17.3 |  7.0 |  0.6  |
  | fork_exit     | 4.94 |  21.0  |  19.7  |  11.4 |  14.3 | 12.1 |  1.2  |
  | fork_exec     | 4.41 |  22.6  |  16.0  |  12.0 |  13.5 | 11.6 |  1.6  |

  The pre-fix placeholder (flush 34.7, store 14.2, hazard 13.9, ifetch 5.3)
  inverts under the corrected counters. **hazard (~21%) is #1 here too** —
  forwarding leads on real code, not just Dhrystone. **`ifetch` is now
  first-class (16-20%) and on the representative fork/exec/pipe paths it is the
  #2 lever, ahead of flush and store** — genuine I-cache/L2 footprint (L1I
  88-94% hit vs Dhrystone's 99.9%), and untouched by any planned gen2.5 feature.
  store swings by workload (22% getpid, ~12% fork/pipe); flush (12-17%) is the
  real BTB target but no longer the headline.

Non-stall cycles equal insns retired (the core never overlaps a stall with
useful work), so a class's cyc/insn is its cycle-share x CPI and the ranking is
exact. On Dhrystone gen2: hazard 0.90, store 0.57, flush 0.36, load 0.19,
funit 0.10 cyc/insn.

What the corrected profile says:

- **hazard is #1 — by a wide margin (28.6%, ~0.90 cyc/insn).** It is RAW
  interlock with no operand forwarding: every dependent instruction waits for
  its producer to reach WB. **Forwarding is the top CPI lever**, not prediction.
- **store is #2 (18.3%, ~0.57).** Write-through round-trips — every store
  reaches L2 (L1-D/L2 write ~88% hit). The store buffer / write-back path is
  the lever here.
- **flush is #3 and genuinely front-end now (11.6%).** Prediction (gen2.5)
  already attacks it and delivers the measured +2.5%; real, but the smallest of
  the three big levers — exactly why BTFN/RAS/BTB moved CPI only modestly (the
  predictors work; the old counter had inflated their apparent target).
- **ifetch ~0, load 6.0, funit 3.3** are minor on this footprint; `ifetch` ~0
  confirms the I-cache (99.9% hit) is not the bottleneck. Re-measure on NetBSD
  before assuming the same — its larger footprint pays real `ifetch` and L2.

Decisions taken from this:

- **Store buffer is in gen2.5 scope** (beyond `overview.md`'s original
  forwarding+prediction list) — co-second cost, and the lever that reaches the
  CPI target on store-heavy code. The uncached/MMIO hazard is resolved by a
  **cacheable-only** buffer: an uncacheable store drains it to completion
  before issuing. That single rule gives MMIO program-order, DMA-kickoff
  visibility (the MMIO write that starts a transfer drains all prior cacheable
  stores first), and trivial load disambiguation (only cacheable loads consult
  the buffer; an MMIO read is a different PTE.C and never aliases a buffered
  entry). It attaches to the existing write-through path — distinct from, and
  lighter than, the gen3 write-back L2 reshape.

- **Build order: prediction -> forwarding -> store buffer.** This is a
  *risk* ordering, not a magnitude one — the corrected profile puts forwarding
  (hazard, #1) and the store buffer (#2) ahead of prediction (flush, #3) in
  payoff. Prediction went first anyway because its blast radius is perf-only
  (EX stays the branch authority, so a misprediction is an extra flush, never a
  wrong result) and it reuses machinery that already exists — the IF1
  `i_redirect` mux and the EX taken-branch flush — making it the safe way to
  prove the variant framework. It has landed (+2.5% DMIPS); **forwarding is now
  the live priority and the biggest remaining lever.** Forwarding second, once the variant framework is proven:
  it is the most result-checkable feature (it changes timing not results, so
  the optimized variant must produce bit-identical output to the baseline
  across the inherited suite — any divergence is a localized forwarding bug).
  Store buffer last (most structural — the load-disambiguation path).

- **Measure each feature's delta as it lands.** Features enter the gen2.5
  fork one at a time (prediction, then forwarding, then the store buffer);
  each one's isolated CPI delta is gen2 vs. the gen2.5 fork at that milestone,
  read off the hardware perfctrs. Composition has no shared file to gate, so
  there are no per-feature compile switches — the milestone is the isolation.

- **Test-signature asymmetry.** Prediction is validated by perfctr deltas
  (flush cycles must drop — its bugs are silent perf); forwarding by
  result-equivalence (its bugs are wrong answers). Running gen2's suite
  against the gen2.5 fork supplies both instruments.

**First cut — ID-stage BTFN + unconditional-direct fold (landed).** Backward-taken/
forward-not-taken on conditional branches, plus always-redirect on
unconditional direct `B`/`BL`. Prediction is at **ID**, not at fetch: the
decoder already produces `OPC_BRANCH` / `cond` / `imm`, so there is no
duplicated pre-decode, and the redirect (`id_pc + imm` -> IF1 PC) runs in a
fresh cycle on registered inputs, off the icache/TLB critical path — it is a
strictly shorter sibling of the existing EX->IF1 taken-branch redirect that
already closes timing, so it cannot become the new limiter. The new pieces are
the direction policy (`penumbra2_predict`), a 1-bit predicted-taken tag ID->EX,
and generalizing EX's redirect from "redirect-on-taken" to
"redirect-on-mispredict". These land in the forked gen2.5 integration files
(the ID/EX assemblies) plus the new `penumbra2_predict` leaf; gen2's own stages
keep resolving on taken, untouched. A correctly-predicted-taken branch costs
3->2 bubbles.

Fetch-time prediction (IF2) was rejected on timing: the target add would sit in
series with the icache hit / way-mux tail, the current fmax limiter. The
real-CPU way to redirect at fetch is a **BTB** — a small RAM, indexed by fetch
PC, returning the target in parallel with the icache (no decode, no add in the
fetch loop) — which is the documented evolution: it buys the last bubble
(3->1/0), keeps fetch-time prediction timing-safe, and (with a RAS) captures
the `JMP R13` returns the real-workload data weights heavily. Start simple at
ID, measure, then add the BTB.

The compiler already emits BTFN-shaped code, so a static predictor hits:
MachineBlockPlacement + the backend's branch-reversal place the likely path as
fall-through and loop back-edges as backward-taken branches — no forward-taken
hot paths, no branch-probability hints (`PenumbraInstrInfo.cpp`
analyze/insert/reverseBranch; `branch-opts.ll`; `Penumbra.td` even sets
`MispredictPenalty = 0`). Direct transfers (`B`/`Bcc`/`BL`) are
foldable/predictable; indirect ones (`JMP` — incl. `RET` = `JMP R13` —,
`JALR`, `BRIND`) need a RAS/BTB. Real NetBSD code has higher call/return
density than Dhrystone (printf/format-heavy call graphs), so the `JMP R13`
return slice of flush is larger there — **the return-address-stack follow-on
likely matters more on real workloads than Dhrystone implies, and should not be
deferred far behind the BTFN first cut.**

### gen2.5 status: BTFN landed (first feature)

> **Removed in the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the ID-stage BTFN predictor's redirect reached back into the fetch path and
> became the fmax limiter, and registering it bought zero cycles (it would fire
> the same cycle EX already resolves the branch). It was removed; the
> fetch-time BTB is the surviving direct-branch predictor. The measurements
> below are retained as the record of what ID-stage BTFN bought.

ID-stage BTFN branch prediction is implemented and committed — the
`penumbra2_predict` leaf, the integration-fork wiring (ID-stage redirect
+ predicted-taken tag; core/spine routing; EX resolve-on-mispredict), and
its unit (`tb_penumbra2_5_ex_stage`) and integration (`test_btfn.s`)
tests. First of the gen2.5 features; gen2 untouched.

**Measured (gen2 vs gen2.5, both at 25 MHz).** Dhrystone CPI 3.03 -> 2.92,
DMIPS 5.03 -> 5.22 (+3.8%), from ~12% fewer flush cycles (85.98M ->
75.76M). The NetBSD pbench kernel microbenchmarks (getpid, clock_gettime,
pipe_pingpong) are ~flat: their flush is return-dominated (`JMP R13`),
which static BTFN cannot predict. This confirms the `penmon`-profile read
above — real-workload flush is call/return-heavy.

**Next (chosen from that result):** a **return-address stack** to claim the
kernel returns BTFN leaves on the table — now landed; see the RAS status
below.

**fmax margin — floorplan deferred on purpose.** BTFN costs ~3.5 MHz:
gen2 synthesizes ~30 MHz, gen2.5 ~26-27.5 across seeds. Confirmed
structural by an A/B — forcing the predictor's two front-end outputs to 0
on the *same* gen2.5 build recovers ~29-31 across seeds, so it is the
BTFN logic, not the build setup. The limiter is unchanged shared logic
(the cache->MMU cone) that merely *places* worse once the BTFN cells +
the ID->fetch redirect bus crowd the memory cluster (utilization ~24%, so
placement, not fullness). gen2.5 still meets the 25 MHz constraint, with
thin margin. A gen2.5 floorplan (`PREPACK_ulx3s_penumbra2_5_top`,
per-TOP, gen2 untouched) is the recovery lever, **deferred until gen2.5
either starts failing timing or has all its features landed** — each
remaining feature loads the same cluster, so floorplanning now would be
redone after each; do it once.

**Tooling / pre-existing.** `NEXTPNR_SEED` is not in the build dependency
graph, so a seed sweep needs `rm build/<top>.config` before each
`make fpga` (synth stays cached). `make test-modules` resolution now
prunes variant-fork dirs (a fork shares the base module name); the gen2.5
EX unit test rides `VARIANT_MODULE_TESTS` with an explicit source path.
Separately, `make test-modules` has pre-existing bitrot unrelated to
gen2.5: `tb_penumbra2_{id,ex,mem,wb}_stage` reference the `o_stall` port
renamed in `dc5dd5c` (to `o_local_stall` / `o_stall_{load,store}` /
`o_funit_stall`), and `tb_penumbra2_irq` / `tb_txn_arbiter` /
`tb_penumbra2_tlb_unit` fail on other committed port drift — fix is
per-testbench reconciliation.

### gen2.5 status: RAS landed (second feature)

> **Removed in the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the RAS rode the same ID-stage redirect that BTFN did, so it left with BTFN.
> Returns now take the full EX-resolve penalty. The measurements below are
> retained as the record of what ID-stage return prediction bought.

A **return-address stack** is implemented and committed — the
`penumbra2_ras` leaf (circular, overwrite-oldest on overflow, DEPTH=8), the
ID-stage call/return detection (a call is any GPR-writing branch/jump —
`BL`/`JALR`; a return is `JMP R13` that is not a call) with issue-gated
push/pop, the predicted-target carry ID->EX, and EX's indirect-target
verification — a return's RAS-predicted target is checked against the
resolved `R13`, sourcing the jump target directly (`i_op_a`, not
`branch_target`) so the added 32-bit compare stays off the ALU-result path.
Returns now redirect at ID over the existing BTFN path (3->2 bubbles)
instead of always mispredicting in EX. Unit test `tb_penumbra2_5_ras`
(LIFO / exact-full / overflow past a full revolution / underflow) and
integration `test_ras.s` (a clobbered-return case exercising the EX target
check); gen2 untouched.

**Fault-shadow drift, accepted.** The stack mutates only on issuing,
non-faulting slots, so a branch mispredict (1-cycle EX resolve) and the
drain-commit redirects (ERET/WRSYS, pipe drained) never corrupt it. A
precise fault is the exception: it commits at WB and squashes an
already-issued shadow of up to two younger slots that may have moved the
stack; those re-run after the handler, leaving a bounded, self-healing
pointer drift. EX corrects every misprediction, so this is perf-only, never
a wrong target — accepted rather than adding a committed-shadow repair
(faults are rare per instruction; the repair would recover noise).

**Measured (RAS off vs on, same gen2.5 build, fmax 27.4 MHz — RAS costs
~0).** Dhrystone DMIPS 7.20 -> 7.29 (+1.25%), CPI 3.09 -> 3.05, from ~1.6M
fewer flush cycles — about one bubble saved per return across ~1.6M returns
(~100% return coverage). NetBSD pbench kernel paths, where BTFN was flat:
getpid and pipe_pingpong -2.8%, clock_gettime -1.6% cyc/op (fork_* too noisy
at iters=1 to read). The RAS reaches the returns BTFN left on the table, as
predicted.

**The flush bucket is real front-end fill, but the corrected counters halve it
and demote it.** This section originally read flush at 37-42% on libc size=1
loops and called the BTB the dominant remaining lever. The carried-cause
counters (`a52ab8c1d1f7`) put those same loops at ~17-26%
(memcpy/memset/strlen size=1) and kernel paths at 12-18%; the missing ~half was
back-end latency tails, now correctly in load/store/hazard. What flush measures
now is genuine — the 2-bubble front-end fill of every taken conditional branch,
which ID-stage prediction (BTFN/RAS) structurally cannot remove and only a
fetch-time **BTB** (target RAM read in parallel with the icache, 2->0/1 bubbles)
reaches. So the BTB stays a real lever; it is just no longer the #1 it appeared
to be, and on the representative fork/exec/pipe paths `ifetch` (the I-cache it
cannot touch) is the larger front-end cost.

### gen2.5 status: BTB landed (third feature)

A **fetch-time branch target buffer** is implemented and committed — the
`penumbra2_btb` leaf (PC-indexed tagged target RAM, default 32 entries,
allocate-on-taken / tag-checked-invalidate-on-not-taken), the forked
integration (core lookup + redirect, `if2_stage` predicted-taken tag carried to
EX, spine training wiring, EX train/verify), unit test `tb_penumbra2_5_btb`, and
integration `test_btb.s`; gen2 untouched. A correctly-predicted taken direct
branch (B/Bcc/BL) is steered at fetch, turning its last front-end bubble into
none — below what ID-stage BTFN/RAS reach. EX stays the authority, so a stale or
aliased prediction costs at most an extra flush, never a wrong result.

**Measured (BTB off vs on, same gen2.5 build, both @ 25 MHz HW).** Dhrystone
DMIPS 7.29 -> 7.48 (+2.6%), CPI 3.05 -> 2.96, flush 9.2% -> 6.1% (-6.6M flush
cycles, -5.0M wrong-path I-fetches). The +2.6% roughly matches the whole
BTFN+RAS step (+2.5%), so the BTB doubles the prediction stack's Dhrystone gain
(7.11 -> 7.48, +5.2% total). The win is workload-concentrated: the hot libc
routines move 3-16% cyc/op (memset/256 -15.7%, memcpy/256 -6.2%, strlen/256
-3.4%), while the kernel syscall paths barely move (getpid -1.9%,
clock_gettime -1.8%, pipe_pingpong flat) — their flush is return/ifetch-bound,
not taken-branch-bound.

**Kept, but on probation against forwarding.** Both builds PASS at 25 MHz, so
the cycle win is banked; the BTB's cost is timing margin, thinned to ~25.11 MHz
on the default seed (placement noise on the shared dcache->MMU cone, recoverable
by the end-of-features floorplan — not chased per-feature). The standing risk is
that **forwarding — the #1 lever, loading the same cluster — may not close
25 MHz on top of the margin the BTB already spent.** If it cannot and the
floorplan cannot recover it, the BTB drops first: forwarding outranks it on
every workload. So retention is conditional on forwarding fitting beside it.

### gen2.5 status: forwarding landed (fourth feature)

Operand **forwarding + regfile write-through** is implemented and committed — the
`penumbra2_forward` leaf (youngest-first EX/MEM-then-MEM/WB select, the GPR
analogue of `penumbra2_flag_bypass`); the forked integration (ID carries
per-operand source tags + the relaxed issue interlock; EX instantiates the
forward network for both operands and *captures* the resolved value across an EX
hold; spine adds the WB->ID write-through mux and the MEM/WB forward source); the
cache-hot result-equivalence tests; and the `pinned-stalls` capability that skips
the gen2 cycle-exact perfctr tests on the variant. gen2 untouched. A reader now
issues as soon as its operand is *reachable* (EX/MEM at d=1, MEM/WB at d=2,
write-through at d=3) rather than stalling to WB; only the irreducible 1-cycle
load-use, plus the deferred SPR-file and divmul-aux cases, still stall. Mechanism
in `doc/internals/penumbra2/hazard-model.md` (gen2.5 forwarding section).

**Measured (forwarding off vs on, gen2.5, HW @ 25 MHz).** Dhrystone DMIPS
7.48 -> 10.16 (**+35.8%**), CPI 2.96 -> 2.17, hazard 30.6% -> 5.6% — the hazard
stall collapsed 7.3x (58.5M -> 8.0M cycles), exactly the #1 lever the corrected
profile named. By far the largest single-feature gain (the whole BTFN+RAS+BTB
prediction stack was +5.2%); total gen2 -> gen2.5 is now +43% (7.11 -> 10.16).

**BTB probation lifted.** Forwarding closes 25 MHz beside the BTB, so the BTB
stays — but timing is thin (a few `NEXTPNR_SEED` values needed to hit 25 MHz; the
capture adds two EX operand latches + an `i_fresh` mux on the operand path). The
end-of-features floorplan is the margin-recovery lever, still deferred until the
store buffer also lands.

**Three bugs, all hidden by an unrepresentative suite — the durable lesson.**
Forwarding only fires cache-hot (uncached fetch spaces instructions past the
pipeline depth, so the regfile always serves the operand), and the one cached
test, `memcpy`, has a single memory op per iteration. So a green suite hid:
1. **WRSYS value bypassed the forward network** — its sysreg-write datum is read
   from the registered `idex_op_b` in the spine, never the EX muxes; a relaxed
   WRSYS wrote stale data and the TLB miss handler re-faulted forever. Fix: the
   WRSYS value operand keeps the conservative stall.
2. **Operand lost when held in EX past the producer's drain** — a consumer held
   by a downstream MEM stall (store burst / slow load) past its producer's WB
   retirement reverted to the stale registered operand. Fix: EX captures the
   forwarded value and retains it across the hold (seeded on the slot's first EX
   cycle from the registered ID-issue signal).
3. **divmul-aux = R0 over-match** — a divmul discarding its high half (Rdh = R0)
   made the aux-match leg stall every R0 reader, which the scoreboard ties valid;
   caught by the subset assertion (relaxed stall must imply the scoreboard
   stall). Fix: the destination-match legs skip R0.
Coverage is now cache-hot, warm-loop result-equivalence tests
(`hw/sim/programs/penumbra2/test_forward_*`, `isa/test_forward*`) spanning
ALU/load/store/divmul/SPR/sysreg producers x op_a/op_b/store-base/store-data/
indirect-target/SPR-read consumers x d=1/2/3 x held-across-drain. Takeaway for any
future bypass work: **test it cache-hot, with the shapes real compilers emit**
(varargs prologues, store bursts, switch jump tables, stack-reloaded returns) — a
cold-fetch suite does not exercise forwarding at all.

**Deferred (intentional).** SPR-file forwarding (ESR/EPC/SCRn — needs a
write-through on each backend) and divmul-aux (Rdh) forwarding keep the
conservative stall; cost is negligible (SPR-scratch is the TLB-miss spill path,
divmul is ~33 cycles).

**Where the work stood when gen2/2.5 was abandoned (post-forwarding,
HW-measured).** These were the next levers; none will be built — gen2/2.5 is
abandoned (see the fmax-closure round below) and gen3 picks up the same
problems from a fresh design. Retained as the record of where the profile
pointed:

1. **Cacheable store buffer** — with hazard crushed, store was the #1 stall
   (Dhrystone 26.1%, ~37M cycles — essentially unchanged in absolute terms, just
   the largest share of a smaller pie; getpid ~22%). The write-through round-trip
   to L2 was the lever — but it could not fit in gen2's remaining fmax margin.
2. **I-cache / L1<->L2 path** — `ifetch` is ~0 on Dhrystone but #1-2 on the
   fork/exec/pipe paths (16-20%), untouched by any landed feature. The levers
   were the overview's 8-16 KB I-cache target and/or the full-line L1<->L2
   transfer reshape (which also cuts the load/store miss tails).

### gen2.5 status: fmax-closure round (37.5 MHz)

A timing-closure round took the gen2 *family* to a **37.5 MHz** operating
point (up from 25 MHz). The ULX3S top's `CPU_HZ` makes the 25 → 37.5 MHz
flip these changes were buying; the flip itself lands once per-board
placement seeds are pinned. The limiter the round attacked was the floor
recorded above — the D-side memory-hit cone plus the cross-module
`L2 → fill_sequencer → arbiter → L1` chain — which a same-cycle
combinational verdict spread across the die; floorplanning cannot fix a
cross-module combinational path, so the fix is to register the boundaries
and shorten the cones.

**Shared-machine changes (gen2 and gen2.5 both).**

- **Fetch enable off the redirect cone.** `o_fetch_en` (→ MMU `i_a_req`,
  → I-L1 `i_en`) gated on `~hold` alone, with the `~i_flush` term dropped
  from IF2's `o_fetch_re`. This severs every redirect source from the
  fetch/MMU launch path; the redirect still writes the registered PC, so a
  redirect target launches one cycle later only when it coincides with a
  hold. CPI-neutral in practice.
- **L1 PLRU touched at fill engage**, off the L2-verdict path (the touch no
  longer waits on `i_l2_busy`). Approximate replacement metadata, so a
  one-cycle-different touch is at most a marginally-different eviction.
- **Fill-sequencer output stream registered.** The L2's per-beat
  data/busy fed the L1 array write + valid set combinationally; registering
  `{o_fill_we, o_fill_word, o_fill_wdata, o_fill_done, o_fill_fault}`
  breaks that cross-module path. Cost: +1 cycle latency per line fill,
  pipelined (throughput unchanged; ~1–2 % of instructions are fills).
- **L2 read verdict registered (`HIT_LATENCY=3`)** on `machine_penumbra2`
  (gen1's L2 instances keep 2). The combinational stage-1 hit verdict
  crossed into the L1 fill install + arbiter; an output flop registers it
  before it leaves the L2. Cost: +1 read-hit cycle and back-to-back L2
  reads serialise (fill-throughput hit on L1 misses, recoverable by the L2
  read-pipeline decouple). Detail in
  [the L2 cache design](../internals/l2-cache.md#pipeline-hit_latency3-the-gen2-machine)
  and [the gen2 memory interface](../internals/penumbra2/memory-interface.md#deferred-fill-speed-directions).
- **L2 read-hit PLRU touch deferred** one cycle (captured at the hit,
  applied against the live array next cycle), off the L2 tag-compare path.
  A read hit never changes state, so the deferred apply always lands back
  in `S_IDLE`; no correctness impact.
- **L1 associativity 4-way → 2-way** on the gen2 machine
  (`machine_penumbra2.sv` `NUM_WAYS=2`; `cache_bram_vipt` still supports
  {2,4} and defaults 4). The parallel per-way tag compares + way-mux were a
  larger share of the hit-verdict cone than expected; halving them shortens
  it and relieves per-way-BRAM routing congestion. Cost: more conflict
  misses (a 2-way index holds half as many lines) — the higher clock more
  than offsets it on Dhrystone.

**gen2.5-only changes.**

- **BTB training write registered.** `o_btb_update` + payload {pc, target,
  taken} were combinationally gated by the dcache hit (via the MEM stall) —
  the same pattern as the old `predict_redirect`. BTB training is
  speculative bookkeeping, so it registers in EX/spine and writes the BTB
  next cycle. No correctness or IPC cost.
- **ID-stage predictor (BTFN + RAS) removed.** `penumbra2_predict` and
  `penumbra2_ras` and their ID→EX redirect/tag ports are gone. The
  predictor's redirect folded in the MEM stall, putting
  `dcache tag → dmem_busy → id_issue → predict_redirect → if2_flush` on the
  gen2.5 critical path. Lifting it off the stall by registering it would
  fire it the cycle the branch is already in EX — the same edge EX resolves
  it — so a registered ID prediction is redundant with EX's own redirect:
  zero cycles saved. Direct branches stay predicted at fetch by the BTB;
  everything the BTB misses (cold direct branches, every indirect jump /
  return) resolves in EX. Result-based `test_btfn` / `test_ras` pass
  unchanged (EX confirmed everything anyway). Cost: returns lose their
  one-cycle prediction and take the full EX-resolve penalty — the accepted
  tradeoff to clear the path. The BTFN/RAS landing measurements are
  retained above under their status entries.

**Open at abandonment.** Recorded only as where the design was left when work
stopped — not as planned work; gen2/2.5 is abandoned:

- Registering the L2↔external-bus boundary (the next fmax step) was blocked by
  a suspected SDRAM-adapter deadlock on gapped reads: a register slice there
  turns a line fill into gapped single reads, and the adapter's
  speculative-prefetch FSM deadlocks on that stream (`test_l2_ifetch` hangs).
- The `HIT_LATENCY=3` register cost back-to-back L2 read throughput; an L2
  read-pipeline decouple would have recovered it.
- Removing the RAS gave up return prediction; only a fetch-domain return
  stack (predicting in the fetch domain, never reaching back from ID) could
  have restored it timing-safely.

**The design is a failure and abandoned.** None of the above will be pursued.

## Hardware: Penumbra/2 machine assembly — done; findings + residue

The gen2 machine is assembled (`hw/rtl/machine/machine_penumbra2.sv`):
both VIPT L1s, the transactional I/D arbiter, the fill sequencer, and
the shared L2 sit behind the core's fetch/dmem front ports; IF2 and
MEM carry their miss-stall handshakes (level-held request, busy-drop
completion, a one-entry IF2 skid for completion-under-ID-stall, and a
redirect-target launch held out of an in-flight I-transaction); the
identity-mapping assertions are gone and the non-identity tests
(`test_tlb_remap`, the COW set) pass; SYSDEV_L1_DCACHE/ICACHE are
wired through the WRSYS commit and RDSYS sideband. The conformance
suite runs end-to-end on `machine_penumbra2_sim`.

Two durable findings from the integration, both instances of one
invariant — *a presented transaction never vanishes mid-flight*:

- **The request/busy mesh must be structurally acyclic.** Every
  memory layer's busy combinationally follows its consumer's request
  (the sync-bus contract), so no request — and nothing a request is
  gated on, like the front-end flush — may combinationally depend on
  any busy, or Verilator (rightly) reports the mesh as a
  combinational cycle. This is why the vector-fetch redirect is a
  registered pulse (captured handler word, one cycle after the read
  completes, `o_active` covering the redirect cycle) and why
  `o_fetch_re` is a pure function of FSM state.
- **Pass-through transactions need an owner, like fills have.** A
  flushed fetch whose pass-through read the arbiter had already
  granted-and-locked would withdraw the back-side request
  mid-transaction and wedge the arbiter's completion condition
  forever (line fills were immune — the cache FSM owns them; the
  window needs a D-access parking the fetch first, which is why plain
  branch tests never hit it). The L1's S_PT state captures an
  accepted-but-busy pass-through read and holds it to completion,
  serving a withdrawn consumer into the void. Front-side consumers
  may therefore walk away from *reads*; writes have no kill source.

Remaining in the gen2 machine, roughly in order:

- First `make timing BOARD=ulx3s CORE=penumbra2 VARIANT=probe` run of
  the machine-shaped probe (it exposes the IF2 tag-compare/way-mux path
  that gated Decision 11's L1-associativity choice; the fmax-closure
  round resolved it to 2-way — see
  [gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)).
- A bus-fault return path through L2/sequencer/arbiter/L1 — **done.**
  Unlike gen1 (no-device wired straight into the core as a sideband),
  the gen2 core is decoupled from the bus by the arbiter's registered
  request and the L1's S_BEAT capture, so the fault must ride each
  layer's completion inward: a `fault` companion travels with the
  busy-drop of a single beat and with the fill-done of a line transfer
  (a faulting beat mid-line aborts the fill). It enters the core on
  both the data (MEM) and fetch (IF2) paths and vectors to
  `VEC_BUS_FAULT` with the faulting vaddr in FAULT_ADDR. The no-device
  fault is the *absence of any slave's claim*, not a fabric-side map:
  each sim slave (`boot_rom` over ROM, `sdram_sim` over RAM) claims only
  the addresses it backs via its own `bus_devsel`, and the fabric
  faults an access nothing claims — matching how the external async
  bus works (ranges are autoconfig-discovered, so no master/fabric can
  hold a static map). An unclaimed address traps rather than alias-
  responding. Tests: `isa/test_bus_fault`, `test_bus_fault_mmu`,
  `test_bus_ignore`, `test_bus_fault_fetch`. The `bus` capability split
  into `bus-fault` (the fault path, now on gen2) and `bus` (the
  SYSDEV_BUS device, gen1-only). Deferred: a fault on L2's *own* line
  fill while L2 is enabled — the only unhandled corner, asserted loud
  in `l2_cache.sv` (reachable only with L2 enabled AND a cacheable
  mapping to an unclaimed address; pass-through faults are the live
  path and abort correctly).
- The remaining capability gaps vs gen1's runner: wrspr, timer, uart,
  machid, `busctl` / the SYSDEV_BUS device — all done (the device lives
  in `machine_penumbra2`, exposing `o_bus_rst` / `o_bus_cfg_en`; gen2
  advertises the `bus` capability and `test_busctl` passes). Remaining:
  the autoconfig daisy chain those bits drive — no conformance test
  exercises it, so it lands with the wrapper-level device-discovery / SD
  path, where `machine_penumbra2_sim` grows an `autoconfig_dev` chain off
  `o_bus_cfg_en`. RAM probing is unblocked by the bus-fault path.

The shared L2 stays untouched; its read-pipeline initiation interval
is the fill-penalty floor, characterised by
`test_back_to_back_read_throughput` in `hw/sim/tb_l2_cache.cpp`.
Deferred fill-speed directions (L2 initiation-interval decouple, wide
datapath, write buffer) are gated on gen2 bottleneck measurements.

L2-side residue of the valid-bit storage design (the L1 half — flop
valids, single-cycle flush — is implemented in `cache_bram_vipt.sv`):
the L2 is the opposite corner — large valid array, no runtime-flush
caller (PIPT) — so its reset clear can stay a sequenced multi-cycle
walk; that, plus uniform hold-core-until-`init_done` bring-up, is
what a multi-cycle reset sequencer is for (reset axis only,
orthogonal to flush; scoped with the L2 rework that de-hacks the
post-reset-walk-while-live).

## Hardware: Penumbra/2 SDRAM-backed sim + EX-frontier interrupts — done; future work

The gen2 RTL sim now drives the **full SDRAM stack** (`boot_rom` over
the ROM region; `sdram_sim` — adapter + CDC + controller + sim PHY +
behavioral W9825 — over RAM, decoded by `bus_devsel`), dual-clocked at
the 4:1 ratio, instead of the never-stall `unified_bus_mem` (89b7ca7;
`unified_bus_mem` survives only in the FPGA timing-probe top). The
conformance suite runs against realistic variable-latency stalls, which
is the point: it immediately surfaced a real interrupt bug a 1-cycle
memory could not.

That bug and its fix (a775e4f): gen2 recognized interrupts at the fetch
boundary and captured EPC from the *speculative* front-end PC, so a
timer IRQ during a cache fill saved a wrong-path EPC and an interrupted
loop resumed past its branch. The interrupt is now a **synthetic fault
the EX stage tags onto its instruction** — it rides the precise-fault
path (EPC ← its own resolved PC, access suppressed in MEM, register
write dropped at WB, re-execute after ERET); the drain FSM, the
fetch-stop, and the boundary-PC capture are gone. The structural payoff:
the EX/MEM boundary *is* the issued/not-issued line for a memory access,
so cutting the interrupt there makes a non-idempotent access
non-speculative for free — no L1 gate, no store buffer. Regressions:
`test_cache_memcpy_irq` (cacheable fill) and `test_uncached_memcpy_irq`
(uncacheable access).

Future work this opened up:

- **External bus bridge + async no-device watchdog.** The combinational
  `(re|we) & ~(OR of selects)` no-device fault is only the *sync* on-chip
  form. A real external/discrete bus has no central decoder, so its
  no-device fault is a no-acknowledge timeout in a bridge — itself an
  autoconfig device fronting a secondary bus. The model is specified in
  [`bus-protocol.md`](../hardware/bus-protocol.md) (e65a1d9); the bridge
  and watchdog themselves are unbuilt.
- **PTE `S` (speculatable) bit — finalize semantics.** Bit 1 (`0x02`) is
  reserved and earmarked (11dfdc8) for an uncacheable-but-idempotent
  override (the boot ROM is the motivating case), with semantics left
  open until the speculation model settles. Not load-bearing today — the
  EX/MEM boundary provides the temporal non-speculation; `S` is a future
  perf lever (it would also ungate the SDRAM adapter's `addr+N` prefetch
  for such regions).
- **CPU-side store buffer (perf, not correctness).** gen2 writes stores
  through in MEM; the EX-frontier cut keeps that correct (cacheable
  replay is idempotent, uncacheable is non-speculative). A store buffer
  would decouple stores from MEM and enable write coalescing — a
  throughput win, deferred. Distinct from the L2 write buffer below.
- **gen2-on-FPGA validation — done.** The gen2 board top
  (`ulx3s_penumbra2_top`) synthesizes, flashes, and runs Dhrystone on the
  ULX3S; both clock domains close timing. fmax findings below.

## Hardware: Penumbra/2 CPU fmax — critical-path findings

gen2 targets 25 MHz CPU / 100 MHz SDRAM on the ULX3S (ECP5-85F sg6).
Unlike gen1's single full-instruction cone, the pipelined core's fmax
floor is the single-cycle **memory-hit cone**: TLB translate (the 2-way
associative match in `penumbra2_tlb_perm`) feeding the VIPT L1 tag compare, once on
the fetch side (port A) and once on the data side (port B). Restructuring
that cone is the gen2 fmax story. Operating point after the work below:
~34 MHz CPU (the D-side memory-hit cone is the structural floor — see
"Campaign outcome" below), ~117–123 MHz SDRAM (single-synth snapshots;
they jitter run-to-run).

### SDRAM domain: floorplanned to recover margin — DONE (`08cbd63`)

The SDRAM controller closed only ~101 MHz (1.5 % over its 100 MHz target)
where the identical RTL reaches ~140 MHz on gen1. The cause was
geometric, not logical: the denser gen2 core fills the central fabric and
crowds the controller into a thin 4-column sliver against the right-edge
SDRAM pads, so its internal routes stretch. A nextpnr `--pre-pack` region
floorplan (`ulx3s_penumbra2_floorplan.py`) corrals the controller and
CDC (`u_sdram.u_ctrl` + `u_sdram.u_cdc`) into a compact block by the
pads → ~117–127 MHz.

Two non-results, recorded so they are not re-tried:
- **Over-constraining** `clk_sdram` via `FREQUENCY NET` did nothing.
  nextpnr's analytical placer always minimizes its cost — it does not ease
  off a met target, so a tighter number only relabels PASS→FAIL.
  Over-constraining helps a path that is *under-prioritized*, not one
  already getting full attention.
- Regions are not expressible in nextpnr's LPF (`LOCATE COMP` only), so
  floorplans are Python `--pre-pack` scripts. Design-intent constraints
  live in a layered overlay (`ulx3s_penumbra2_design.lpf` + the pre-pack
  script), never the vendor board LPF — a file that can only speak
  REGION/FREQUENCY cannot misclaim a pin.

`keep_hierarchy` on the TLB cone + pipeline stages (`245aff2`) both made
timing reports legible (real net names, not post-flatten gibberish) and
recovered placement the dense core had scattered.

### CPU domain: the stall ripple, decoupled — DONE (`b5cb675`)

The CPU critical path was a **combinational stall ripple** spanning the
whole pipe: a load/store's TLB + D-cache hit check drives `i_dmem_busy`,
which propagates `mem_stall → ex_stall → id_stall` back to the fetch
enable in a single cycle (pure-stall back-pressure). About half of it was
routing — the stall wire physically crossing the die from MEM to IF1.

Fix: a 2-entry elastic FIFO (`penumbra2_fetch_buffer`) at the IF2→ID seam.
IF2 now back-pressures on the buffer's *registered* `o_enq_ready` instead
of `id_stall`, so the back-end stall no longer reaches `o_fetch_en`
combinationally. Cost: one IF2→ID cycle — an extra wrong-path slot,
flushed with the front end on the shared `if2_flush`. The buffer is
generic + unit-tested; the integration passes the gen2 conformance suite.
Sim + synthesis only — not yet flashed-and-run on the board.

### Standing lever: the I-side PLRU update sits on the hit path

After the buffer, the limiter relocated to the symmetric *fetch-side*
cone — proof the buffer did real work, not placement noise. The path
(31.29 ns, `a_asid_q → … → u_icache.plru[*].CE`):

```
  TLB port-A match cone (u_perm_a)     9.3 ns
  TLB → MMU paddr                       1.7 ns
  I-cache tag compare + way-mux         7.4 ns
  PLRU replacement-bit update          12.4 ns   ← 40 %
```

The surprise: the **PLRU update is 40 % of the path**. In
`cache_bram_vipt`, the per-set tree-PLRU is touched on a read/write hit in
the *same cycle* as the hit, so `rd_hit_resolve` (= the deep `hit` signal)
gates the write-enable of the per-set replacement registers and fans the
hit cone across all 64 sets. But PLRU bits are metadata consumed only on
the *next miss* (victim pick), and PLRU is approximate — they have no
business in the hit cycle.

**Lever — defer the touch (DONE, `e9ad4b6f37b5`).** Register
`{touched, set, way}` at the hit; apply
`plru[set] <= plru_update(plru[set], way)` the next cycle against the
*live* PLRU array. Design points to get right:
- back-to-back touches to the same set must compose (apply against live
  `plru[set]`, not a snapshot — then the second update sees the first);
- a fill-completion vs deferred-touch collision at the apply cycle needs a
  priority rule;
- cost is one access of staleness on a victim pick — invisible in hit
  rate, since PLRU is already approximate.

It lives in the shared `cache_bram_vipt`, so it helps the D-side cone too.
The ~19 ns / ~50 MHz projection did **not** hold: the defer worked but
exposed co-equal masking paths, and the real floor is the D-hit stall cone
at ~29 ns / ~34 MHz. See "Campaign outcome" below for the full arc.

Caveat on the rollup, same as gen1: nextpnr attributes fused
post-flatten LUTs by net-name prefix, not dataflow, so per-module labels
can mislead (the I/D `penumbra2_tlb_perm` instances especially). Trust the hop trace
and the start/end points.

### Campaign outcome: the D-side memory-hit cone is the floor (~34 MHz)

Past the stall-ripple and PLRU work above, the limiter walked through the
rest of the memory subsystem; the deferrals/retimings that landed (all
gen2 conformance-clean, sim + synth):

- **Main TLB → async LUTRAM + verdict register** (`c7d7d7e0`): the BRAM
  TLB forced the whole translate cone into the resolve cycle; async
  distributed-RAM storage (the gen1 `tlb.sv` recipe) runs it
  combinationally in the launch cycle, verdict registered once at the MMU
  output. Collapsed three register sets (BRAM read, pinned alignment,
  query capture) into one. CPI-neutral (same 2-cycle contract). The gen2
  MMU stack was subsequently renamed off the misleading `_bram` suffix and
  moved to `penumbra2/` (`penumbra2_mmu` / `_tlb` / `_tlb_unit` /
  `_tlb_perm`).
- **L1 fill install deferred** (`cb6a4452`): tag/valid/PLRU install moved
  off the L2-hit-driven `i_fill_done` into the already-existing S_SERVE
  cycle. Zero-cost — nothing in S_SERVE reads the freshly-installed line.
- **MMU port-A translate degated** (`f1203d58`): after the async TLB,
  port A's lookup-enable (`i_a_req && mode`) put the late I-side fetch
  request — carrying the IF2→IF1 back-pressure off a busy I-cache — on the
  verdict cone. The per-request gate was redundant with the verdict
  register CE, so dropped for port A (no sysreg duty; read address is
  always the PC). Port B keeps it: `i_b_lookup_en` also selects the
  port-B readback address and backs the contention guard. CPI-neutral.
- **Arbiter transaction-aware completion** (`aa55a768`): defer only the
  line-fill re-grant (beats stay back-to-back), keeping the L2 hit verdict
  (`i_fill_done`) off the arbiter's same-cycle re-grant path. Cost: +1
  cycle per back-to-back line fill (≈0 compute-bound, ~7–10 % pure
  streaming; recoverable later by a wider L1↔L2 datapath). Worth ~1.9 MHz
  at the final config — without it the L2→arbiter path caps fmax at
  32.35 MHz. (It was briefly reverted on the mistaken read that it bought
  nothing — see the first lesson below.)

Net fmax: ~33.5 → ~34.2 MHz (≈ +2 %). The headline barely moved; the big
intermediate swings (down to ~31.5, back up) were mostly the placer
redistributing near the wall. The durable value is the structural cleanup
and knowing where the floor is.

> **Superseded by the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the floor below was measured with a 4-way L1 and the L2 read verdict
> combinational. Halving L1 associativity to 2-way shortened the tag-compare
> leg, and registering the L2 read verdict + the fill stream lifted the
> cross-module limiter off this cone — the gen2 family then closed 37.5 MHz.
> The two-halves analysis still describes the *shape* of the residual D-side
> cone; the absolute numbers predate those changes.

**The floor** is the D-side memory-hit cone — D-cache 4-way tag compare →
`dmem_busy` → stall network → ID `drain_commit`, ~29 ns — in two halves,
both fundamental:
- cache hit determination ~10 ns: BRAM tag clk-to-Q + the 4-way compare,
  routing-bound across the per-way BRAMs;
- stall→commit tail ~12 ns: pure-stall requires the hit verdict to gate
  commit the *same* cycle; the flatten removed the ripple, but the signal
  still crosses MEM→spine→ID.

Reaching this floor needs the arbiter change above; it is otherwise not
deferrable — the compare is a BRAM read (L1-D tags ~5.5 Kb could go async
LUTRAM, but the compare is bounded by the registered paddr arriving at
resolve, so it would not help), and the stall→commit gate is the in-order
pure-stall tax.

**Lessons (do not re-run this campaign blindly):**
- Re-measure a change's fmax value at the *final* config, not by its
  mid-campaign delta. The arbiter measured as "noise" (33.87→33.44) while
  a co-equal path masked it, but became worth ~1.9 MHz once port-A cleared
  that path — which is why reverting it dropped fmax to 32.35, not the
  ~33.9 the stale delta implied. A fix is worth the gap to the *next*
  path, and that gap grows as you clear the ones above it.
- Performance is fmax × IPC; ceiling you don't clock into is worthless —
  fix the operating clock first, then judge CPI-for-fmax trades against
  it. The arbiter's +1/fill is worth it *here* only because headroom is
  the goal; at a clock far below the ceiling it would be pure loss.
- The deferrals cleared *masking paths* — incidental cones near the floor
  that retime away. The D-hit cone was always there; it surfaces once the
  masks are gone. Targeted fixes converging on a path present from the
  start = the floor, not another obstacle.
- Deeper fmax needs a microarch change (forwarding / non-blocking loads;
  OoO), not more single-cycle deferrals. Forwarding + branch prediction
  will reshape the EX/MEM/stall/commit region — exactly the floor — so
  budget those designs timing-aware here; don't pre-optimize the stall
  tail, it will be rewritten.
- nextpnr labels fused LUTs by net-name prefix, not dataflow: "the path
  goes through X" repeatedly meant a LUT *named* X (`wrsys_resync`,
  `ac_spi_sel` were registered/unrelated signals), not X's logic. Confirm
  against the RTL before trusting a per-module label.

Operating-clock note: the campaign cleared headroom to bump the CPU PLL
from 25 → 30 MHz (≈ 14 % margin under the ~34 MHz floor, room for the
forwarding/BP logic to eat into). That PLL change + a flash-and-run on the
board is the remaining step.

### Dead LUT-RAM in the main TLB's port-A copies (open; a gen3 lesson)

`penumbra2_tlb` keeps each entry's 64-bit `{PTE, VPN}` word in four
LUT-RAM copies, one per way for each translate port, and every write
updates all four. Port B's copies also serve sysreg readback, but port
A's feed only `penumbra2_tlb_perm`, which reads VPN, ASID, PPN and the
G U X W R C flags. The rest of port A's word is written on every fill
and never read:

| Stored bits | What | Why it is never read |
|---|---|---|
| VPN word [31:28] | pad | the match ignores it |
| PTE [11:8] | SW bits | readback comes from port B |
| PTE [1] | reserved flag | unused |
| PTE [0] | V | validity lives in the `way*_v_vec` flop vector |

V is dead in all four copies: readback substitutes the flop vector too.

Synthesis would normally trim unread memory bits, but port A's read
word leaves `penumbra2_tlb` through `u_perm_a`'s word-shaped ports, and
`penumbra2_tlb_perm` is `keep_hierarchy`: the TLB sees the bits as
used, the permission module drops them, and neither side sees the
other's half. Measured on the gen2 and gen2.5 synthesis netlists
(identical; the MMU is shared): 8 LUT-RAM cells, the pad and SW columns
of both ways, plus about two dozen LUTs, mostly their read muxes. Tiny
as area, but it sits in the translate cone: the dead cells add 12.5 %
to the read-address load of port A's LUT-RAM (the I-side set index,
`i_a_vaddr[16:12]`) and are placed in the congested MMU region.
`make fanout` lists their inputs under `(no register)`.

Not fixed, since gen2/gen2.5 take no further core work. The fix, if
ever wanted: give `penumbra2_tlb_perm` field-shaped ports (VPN, ASID,
PPN, flags) so unused bits never cross the boundary, and drop V from
the RAM.

gen3 lesson: `penumbra3_tlb_store` and `penumbra3_tlb_verdict` pass the
same word-shaped `vpn_word` / `pte_word`. Neither carries
`keep_hierarchy`, so synthesis still trims across them; if the verdict
gets a kept boundary for timing, switch its ports to fields in the same
change.

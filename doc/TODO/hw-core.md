# Hardware: cores, memory system and timing

CPU cores (gen3 build, cross-generation ISA work), the bus, caches
and SDRAM path, and fmax / placement findings. Kept in one file
because these items gate each other: memory-system work is sized
against the core generation and the logic budget timing leaves.

## Hardware: SDRAM adapter may deadlock on a non-back-to-back read stream (suspected)

Found while probing the gen2 path toward 50 MHz. A single-outstanding
register slice inserted on the L2 <-> external-bus boundary (in
`machine_penumbra2`, to cut the busy-form bus's combinational `L2-addr ->
bus-decode -> busy -> L2` round-trip — the registered `ack` removes that
round-trip without a slice) turns the L2 line fill into four *gapped*
single reads — `re` drops between words, +2 cycles each. With that
slice in place execution ran ~174 instructions and then wedged:
`test_l2_ifetch` hung at cycle 1448 ("no program end within 2000000
cycles"). Single bypassed reads (boot, MMU off) and the normal
back-to-back fill both work fine — only the gapped-mid-fill stream
deadlocked. Behind the fabric watchdog the same wedge would end in a bus
fault after `TIMEOUT_CYCLES` rather than a hang (inferred, not
reproduced); inside an L2 fill that trips the L2's fill-fault assertion.

Suspected cause in `sdram_bus_adapter.sv`'s speculative-prefetch FSM. Its
own header says a mispredict (next read != the speculated `addr+4`) is
handled by abandoning the in-flight spec and discarding its response — so a
*deadlock* on a gap is a robustness bug, not an inherent limit. Likely
culprits: the "in-flight hit, stall in BEGIN" case never getting its
consume when `re` goes low mid-spec, or the 1-bit tag FIFO desyncing when a
spec response is orphaned by the gap. This is the same family as the
CDC handshake-skew bug fixed in `db683a71b9b5` (speculation interacting
badly with a changed/interrupted request stream), so the spec state machine deserves a
hard look for any remaining "assumes the master streams back-to-back"
assumption — the master is not obligated to.

Not currently triggered: neither the gen2 L2 nor the gen3 bus master gaps
mid-fill -- both hold `re` and advance the address word-to-word. The gen3
CPU<->bus decouple (`penumbra3_bus_master`) is exactly the pipelined master
that keeps the back-to-back stream, and its Phase-0 probe P0.4
(`penumbra3_bus_master_test`) confirms registering the boundary that way
does not hit the deadlock -- so this does not block gen3.

The bug itself still stands and should be fixed. The bus contract does not
oblige a master to stream back-to-back, so a gapped read stream is a valid
sequence the adapter must serve rather than hang on. The fix lives in the
adapter's speculative-prefetch FSM: it must abandon an orphaned spec on a
gap exactly as it already does on an address mispredict. File:
`hw/rtl/io/sdram/sdram_bus_adapter.sv`.

Scope: the word stack only. The line stack
(`hw/rtl/io/sdram_line/sdram_bus_adapter.sv`) speculates on nothing and
keeps one request outstanding, so it has no orphaned-spec state to
desync and cannot reach this wedge — a gap between reads is just a gap.
A top's stack is pinned beside its placement seed, so which tops this
entry covers follows `SDRAM_STACK_<top>` in the Makefile rather than
being listed here.

## Hardware: the SDRAM controller waits out `T_WR` after every write

`sdram_ctrl` issues a write's second half-word in `S_RW`, then counts
`T_WR` down in `S_RECOVER` before returning to `S_IDLE`, which takes
one more cycle to accept the next request: six SDRAM clocks for each
word written to the open row. `T_WR` is the wait between a write's
last data and a `PRECHARGE`. A `WRITE` or `READ` to the open row may
follow at once, so only a precharge — for a row change or a refresh —
needs to wait it out.

Measured, it binds nowhere yet. An isolated store never sees it: the
adapter releases a write once its CDC accepts it. In a stream of word
writes the CDC's two slots recycle one every ~3 CPU cycles at
37.5 MHz, and the controller's 2.25 fit under that
(`doc/internals/penumbra3/memory-performance.md`, "Sustained writes on
the SDRAM stack"). A line written as one request takes 12 SDRAM
clocks, 3 of them this wait, but through the bus the line's four
words take 8 CPU cycles to arrive and the controller's 4.5 fit under
those too.

It binds on a native port for on-board RAM, where the controller paces
a line write: ~4.5 CPU cycles a line, ~3.4 without the wait (same
file, "SDRAM side: write combining"). A deeper CDC would expose it on
word streams as well. The controller is shared by both SDRAM stacks
and every top.

Open:

- Count the write recovery separately, return to idle straight after
  the write, and have the precharge paths wait for the count. Worth
  doing with the native port or a deeper CDC, not before.
- The idle cycle that accepts each request is a smaller, separate cost
  per request.
- Regression: `sdram_model` checks command legality but no timing
  parameter. It needs a write-to-`PRECHARGE` check, and
  `tb_sdram_test` a stream of writes to the open row that asserts the
  commands follow back to back.

## Hardware: gen3 (Penumbra/3) Phase-1 build status

Phase 0 (the four structural probes) passed; see
`doc/internals/penumbra3/phase0-probes.md`. Phase 1 builds the 7-stage
skeleton stage-by-stage: each module is forked or written, given a
`MODULE_TESTS` unit test, and committed to `main`. Run one test with
`make sim MOD=<name>_test TB=tb_<name> PROG=` (a packed-struct port needs a
`_test.sv` unpacking wrapper; flat-port modules use the bare name).

**Done** (front end through memory access, all unit-tested):

- `penumbra3_pkg` control bundle + enums (`7a964ee`); `dpath_payload_t`
  pipeline-register payload (`d8c2d5d`)
- `penumbra3_decode` -- word->bundle, parallel-then-select, mode-independent (`4e60d01`)
- `penumbra3_fetch_buffer` -- IF2->ID elastic FIFO (`00051be`)
- `penumbra3_if1_stage` (`67eeb41`), `penumbra3_if2_stage` decode-on-enqueue (`27166b0`)
- `penumbra3_id_stage` -- regmap + scoreboard + issue + priv/fault finalize (`10e088b`)
- `penumbra3_ex_stage` -- operand forwarding + ALU + flags + branch + divmul +
  drain-commit, with `penumbra3_alu` / `penumbra3_flag_bypass` leaves (`25d7b28`)
- `penumbra3_mem1_stage` / `penumbra3_mem2_stage` -- split launch/resolve: TLB
  verdict + cache hit, sub-word extract, fault merge, the `mem_completion`
  hold buffer; `mem_completion` parks the full descriptor and
  `dtranslate`/`tlb_store` gained an `i_hold` read clock-enable for the
  freeze; composed mem1->dtranslate->mem2 test (`96da15b`)
- `penumbra3_wb_stage` -- the commit point: physical-index write routing
  (regfile vs SPR-file/scratch, USP-to-regfile), the NZCV flag strobe, divmul
  dual-write sequencing through the single regfile port, and precise
  fault-commit gating; flat-driven unit test (`79ef076`)
- `penumbra3_regfile` / `penumbra3_spr_file` / `penumbra3_scratch_file` -- the
  storage tier WB commits into: GPRs + banked USP/SSP (R0 forced 0), SR/EPC/ESR
  (SR moves only via entry/ERET/EI/DI + flag commit -- no WRSPR SR leg), and
  SCR0..3; each flat-port unit-tested (`3e40ce4`)
- `penumbra3_spine` -- ID/EX/MEM1/MEM2/WB integration around the
  regfile/SPR/scratch files: bundle+payload handoffs, operand/flag forward
  sources into EX, the scoreboard clear, the regfile write-port mux (commit vs
  load completion), drain-commit (ERET/EI/DI, WRSYS) + fault commit/flush, and
  the freeze distribution (registered `mem_pending` freezes the launch side +
  issue; the MEM2/WB register is held only by WB back-pressure -- the carve-out).
  Fetch-stream unit test: ALU forwarding, divmul dual-write, precise fault
  (`297b12d`)
- `penumbra3_vecfetch` -- exception vector-fetch FSM: borrows the fetch port on
  a fault commit, reads `vector_table[vec<<2]` (MMU-bypassed), redirects IF1 to
  the handler from a register (`19cb214`)
- `penumbra3_irq` -- interrupt recognition + EX-frontier injection (synthetic
  fault on the precise path; EI one-instruction shadow) (`e19a10a`). The spine
  gained `o_ex_valid`/`o_ex_stall` -- the clean-boundary frontier the unit gates
  on (`94d0b9a`)
- `penumbra3_core` -- the front end (IF1/IF2 + fetch buffer) onto the spine plus
  `vecfetch` + `irq`: the fetch-port mux (IF1 vs vecfetch), the redirect/flush
  composition (branch / vector-fetch / ERET / WRSYS-resync steer; a fault
  flushes but steers later), and the I-fetch + D-side port groups exposed for the
  machine layer. Integration unit test (sync-read fetch ROM): streaming +
  in-order commit, branch redirect, interrupt entry via the vector fetch
  (`c281f88`)

**Spine follow-ons** (do not block the memory-hierarchy fork): the
*forwarding-release* increment
(early scoreboard clear + the `i_fwd` broadcasts -- correctness-first leaves
them tied off today, so a load/divmul-use waits for the registered clear).

**Memory hierarchy** (the fork in progress):

- `penumbra3_translate` -- the side-neutral translate path (renamed from
  `dtranslate`; the MMU uses it twice), now exposing its registered set for
  sysreg read-back (`9fd2928`, `460f6d4`)
- `penumbra3_mmu` -- I+D translation unify: `translate` x2 over a shared
  coherent write stream + dual-port `tlb_pinned`, per-port bypass>pinned>main
  verdict, the device-0 sysreg interface (MMUCR / FADDR / FSTAT / TLB
  install+read-back), commit-time fault registers (`02f57e3`)

**Next, in order** -- the shapes `doc/internals/penumbra3/overview.md`
ranks first ("Levers ranked by measured benefit"):

1. **The L1 caches:** `penumbra3_l1` is built -- storage, the hit
   verdict, the store-hit write with its forwarding, tree-PLRU, the
   enable, the cache device's sysregs and counters, 8 KB 2-way on the
   ECP5, and no FSM, since miss orchestration is the consumer's. Both
   sides reach memory through the port
   `doc/internals/penumbra3/uncore-port.md` specifies. **The D-side**
   has it: MEM1 launches the L1, MEM2 consumes the verdict, and
   `mem_completion` parks a load miss with the L1's victim way and every
   store, holds the request up on the port, installs a load's line and
   completes the access, taking a bus fault precisely. Still open
   there: a register for `store_fault`, which `doc/system` does not
   define, so the port's pulse goes nowhere. The fetch side still has
   `i_fetch_busy`.

   **The fetch side:** P0.5 settled the lookup and the IF2 shape
   (`doc/internals/penumbra3/phase0-probes.md`, "P0.5 -- physical-tag
   compare behind the TLB verdict"); the real IF2 takes it from the
   probe top: each way's word decoded and pre-decoded beside the
   compare, the hit selecting. It moves to replay
   (`doc/internals/penumbra3/timing-outline.md`, "4. Caches"):
   today IF2 reads busy low as word valid and IF1 folds busy into its
   hold and the block RAM enable, which puts the I-cache hit on that
   cone. Under replay a miss sets a flop that steers IF1 back to the
   missed PC next cycle, and IF1 holds only on FIFO-full or a
   registered fill; an uncached fetch, which never hits, then needs its
   word delivered to the replayed slot. P0.5 never saw the hold form
   as its worst path, so the full-top read is where replay has to show
   its worth.
2. **The uncore pipeline:** L2 -- write-through and posted, a look-up
   every cycle, the line-wide transfer with fill-through -- with the
   I/D select and the fill folded in, driving `bus_master`.
3. **`machine_penumbra3` and `ulx3s_penumbra3_top`.**

**Before the TLB store's geometry is fixed:** the proposed 256x2 needs
`MMU_CONFIG` version 2 and a wider `TLB_INDEX` in `doc/system/mmu.md`
(evidence: `doc/internals/penumbra3/memory-performance.md`, "TLB: sets, and a
cheaper refill").

Phase-1's gating checkpoint is then the **full-top composition timing read**
(bare skeleton, no BTB) -- calibrates the probe->full margin before feature work.

**Locked hazard-model decisions** (context for resuming):

- Decode runs on the FIFO *enqueue* path and is a pure function of the
  instruction word -- everything mode-dependent (regmap, the privilege fault,
  the fault vector's `VEC_PRIV` arm) stays in ID against live `SR.S`.
- The bundle carries a single positive-sense `dst_we` (routed GPR/SPR by
  `dst_is_spr`): a forwarding machine's destination-valid bit must mean
  "actually writes", so a CMP must advertise no destination.
- The scoreboard tracks only the non-forwardable producers (LOAD / DIVMUL /
  RDSYS); ALU results never set a bit -- EX forwards them. The issue gate
  blocks a source only on `used & pending & ~forwardable`.
- Forward broadcasts to ID are producer *stages* (not source lanes), so every
  source matches every broadcast slot.
- The fetch FIFO *is* the IF2/ID register (decode folds into enqueue, no extra
  stage); the IF2 skid is retained for the drop-equals-valid I-cache
  completion when the FIFO is momentarily full.
- The resolved datapath rides a packed `dpath_payload_t` (value, dest routing,
  flags, PC, fault verdict) carried as a unit across the EX->MEM->WB registers
  alongside `ctrl_bundle_t`; `store_data` rides its own EX->MEM1 wire (consumed
  in MEM2, never reaches WB).
- The back-end stall is spine-distributed: MEM2's `mem_completion` produces
  the registered `mem_pending`, and the spine fans it back to freeze the
  *launch side* -- MEM1, the MEM1/MEM2 register,
  `dtranslate`/`tlb_store`/cache, and issue -- so the missing load and
  everything younger hold. It must NOT freeze the MEM2/WB output register:
  that drains older work to commit (the held load retires via the completion
  path, not MEM2/WB), and folding `mem_pending` into its freeze re-presents an
  already-committed multi-cycle writeback (the divmul dual-register retire)
  and retires it twice. The MEM2/WB register's only back-pressure is the WB
  dual-write hold. Contract:
  [load-completion freeze scope](../internals/penumbra3/memory-completion.md#what-the-pipe-gate-freezes).
  A frozen MEM2 slot keeps its own verdict, and no combinational cache/TLB
  verdict ever reaches issue. Candidate spine assertion: `mem_pending` never
  gates the MEM2/WB register.

**Deferred (resolve at the named task):**

- *Issue-release broadcasts* -- the operand-value forward network is settled
  (EX sources EX/MEM1, MEM1/MEM2, MEM2/WB; youngest-wins; write-first regfile
  for the four-deep distance; the MEM1 and MEM1/MEM2 legs exclude loads and
  sysreg reads, which forward from MEM2/WB). What remains is a pure spine
  task: drive ID's `i_fwd*` to release a scoreboard-pending load / sysreg read
  at the cycle its value reaches MEM2/WB -- gated on the registered
  `mem_pending`, never a live verdict. ID owns only the match today.
- *divmul aux-destination scoreboard bit* -- set path done (`4fa078b`): the
  scoreboard has a second set port, ID regmaps the aux (Rdh) and sets its bit at
  issue; the spine drives the matching clear at the aux writeback.
- *divmul writeback back-pressure reach* (`wb_local_stall`: the WB dual-write
  ripple WB->MEM2->MEM1->EX->ID). The divmul's two-cycle WB writeback makes WB
  occupy two cycles, and that hold ripples up the pure-stall chain. It is 1-bit
  and flop-shallow -- no cache/TLB verdict in it, so it is correct and within
  the "1-bit ready crosses backward" allowance -- but it is the longest-*reach*
  combinational stall and may bind at 50 MHz on routing distance alone.
  Floorplanning is NOT the remedy (placement cannot be the fix for a structural
  reach). Structural fix, deferrable because it is localized and does not touch
  the core shape: retire the divmul writeback through the **load-completion
  path** rather than the WB stage. divmul holds in EX until it is the oldest
  in-flight -- `drained` = `~mem1_valid & ~mem2_valid & ~memwb_valid &
  ~mem_pending` (the drain-commit condition extended with `mem_pending`) --
  then writes Rd/Rdh over two cycles through the completion write-port mux. The
  port is free **by the in-order single-outstanding invariant** (oldest => no
  older writer; younger blocked behind the held divmul), NOT by any cycle count
  -- so a slow uncached MMIO access ahead of the divmul simply makes it wait in
  EX, never write early or out of order. WB then never sees a divmul: the
  dual-write FSM and `wb_local_stall` both delete, and back-pressure reduces to
  the `mem_pending` fan-out plus the one-hop EX->ID stall. Land as its own
  increment if HW timing shows the reach binds; until then the WB dual-write is
  correct (WB is the oldest by pipeline position, so it is already drain-gated
  -- only the reach is suboptimal).
- *Vector-fetch cacheability* -- `penumbra3_vecfetch` will keep the vector
  read uncached for now. It is the CPU's only fetch-by-PA (not VA), so
  threading a PA index into the VIPT caches is awkward; MMU-bypass is required,
  cache-bypass is not, so making it L2-cacheable is a worthwhile later win
  (kills a full-SDRAM round-trip per trap). Revisit after the skeleton boots.

## Hardware: FPU

Add a floating-point unit to the ALU. Currently using soft-float.

## Hardware: gen1 CPU fmax — critical-path findings

gen1 targets 25 MHz on the ULX3S (ECP5-85F sg6). It is the single-cycle
microcoded core, so one combinational cone spans the whole instruction:
micro-ROM read → control decode → register-address resolution →
register-file read → ALU → flag update. There is no slack cycle to hide
any of it in — that cone is the structural fmax floor for this
generation.

### RDSPR-USP read-address override removed — DONE (`82d758cb4689`)

The register read-address path carried a per-instruction mux that forced
the A address to R14 at runtime, solely so RDSPR USP could read the
banked user stack pointer. Its select came from the late micro-ROM
`a_src` field, so every instruction's read address waited on a decode
that mattered for one rare op. Moving the R14 selection into the `rdspr`
micro-word (`reg_a=R14`) makes the address a static literal: the
register file already maps R14 to the banked SP and `cross_bank` still
picks USP vs SSP, and the A-mux ignores `reg_a` for the other SPR
sources, so one literal address is correct for every SPR variant. A
`datapath.sv` assertion guards the microcode-supplied-address contract.

Measured on ULX3S, single synth run each:

```
  gen1 CPU fmax:  25.37 MHz → 27.11 MHz   (constraint 25 MHz)
  critical path:  39.42 ns  → 36.89 ns
```

### The bottleneck relocated — read the path shape, not the number

The durable lesson. Before, the critical path was the compute cone,
rooted at the micro-ROM and ending at the `flag_z` register data input.
After, it is a different net entirely:

```
  PC reg → MMU → TLB pinned-match (~11 ns) → TLB main → D$ cacheable
         → I$ tag compare (~4 ns) → read-miss → bus arbiter
         → sequencer next_upc → flag write-enable (flag_z clock-enable)
```

i.e. the instruction-fetch / address-translation / hit-miss control
path. The compute cone dropped clean below a path that had been sitting
just under it the whole time. Two consequences:

- It proves the change was real signal, not placement noise. Noise
  jitters the number on the *same* path; it does not relocate the
  startpoint or move the limiter to another subsystem. A changed path
  shape is the honest test for "did this optimization do work."
- Further compute-cone work (the 32-bit `flag_z` zero-detect reduction,
  the register file, the ALU) now buys **zero** fmax — none of it is on
  the limiting path. The next gen1 lever would be the TLB pinned-match
  and I-cache tag compare, which is exactly what the gen2 pipelined
  fetch restructures, so it is not worth chasing in gen1.

Caveat on the timing tool: the nextpnr per-module rollup attributes
fused post-flatten LUTs by net-name prefix, not by logical dataflow. The
two register-file read ports (A/B) are independent parallel reads of
replicated DPRAM banks, but because they feed a common ALU sink the
rollup can make them *look* serial. Trust the hop trace and the
start/end points, not the module labels.

### RTL-cleanup-for-fmax techniques (ref: openiphub UberDDR3 post)

[Pushing UberDDR3 frequency through RTL
cleanup](https://www.openiphub.com/post/pushing-uberddr3-frequency-through-rtl-cleanup-post-18)
walks a DDR3 controller from 82 → 132 MHz. The transferable techniques,
and how they land here:

- **Register control decisions one cycle early; reuse the registered
  decision instead of recomputing it combinationally.** Their biggest
  wins. For a single-cycle microcoded core this *is* adding a decode
  stage — there is no earlier cycle to register into — which is the
  gen2 premise, not a gen1 tweak.
- **Don't gate the common path with rare-command logic.** Exactly what
  the RDSPR-USP override removal did: a rare op sat on every
  instruction's read address.
- **Flatten deep control trees; right-size register/counter widths;
  separate combinational from sequential.** Cheap, local, low-risk — the
  right tools for buying placement-noise margin rather than raising the
  ceiling.

The headline: gen1's ceiling is structural (one cycle does fetch +
decode + read + execute + writeback), so the durable fmax lever is the
gen2 pipeline. Within gen1, prefer small depth-shaving wins that buy
safety margin over the 25 MHz target rather than campaigns to raise it.

## Hardware: L2 phase 2 — write-back / write-allocate

The headline remaining cache optimization, and the highest-impact
item left in this section.  Today every L1-D store under WT-WnA
still pays a full SDRAM round-trip — Dhrystone shows 1.22M writes
per run, each ~20-30 cycles, ~25-37M cycles of write traffic on a
45M-cycle run.  Phase 2 absorbs writes into L2 entirely until
eviction.

The per-store figure predates `doc/system/instruction-timing.md`,
which puts a write-through store at a typical 8 cycles on penumbra1
and 6 on penumbra2_5; re-measure before sizing the win on it.

Upper-bound win (from current measurements): the 841K Dhrystone
write hits become zero-bus-traffic dirty marks (~25M cycles saved
on Dhrystone alone, before counting kernel workload gains).  WA
additionally captures the 380K cold writes at the cost of one
read-fill per first-touch line — net positive when intra-line
write locality is high (stack frames, page zeroing).  pbench data
shows the workloads WT-WnA didn't help (`fork_exit`, large
`memset`) are precisely the ones that have heavy cold-write
traffic, which WB+WA targets.

Implementation outline (full design in
`doc/internals/l2-cache.md` § Write Policy / Phasing):

- Per-line dirty bit (4096 bits at 64 KB / 16 B lines)
- Stage-1 write hit sets dirty (no bus traffic)
- Eviction of dirty line writes back before installing new line
- `FLUSH_LINE` / `FLUSH_ALL` sysreg ops for software-managed
  coherence (before other hardware reads memory, etc.)
- WA path: write miss allocates the line, then dirty-marks it
- Optional phase 3: 1-deep writeback buffer to overlap eviction
  drain with the new line fill

Estimated complexity: ~250-300 lines of RTL + dirty bit storage,
plus FLUSH ops and the eviction state machine.  Test surface adds
write-hit-no-bus-traffic, dirty-eviction-writes-back, and
flush-then-evict-is-clean assertions.

Forward-looking note on perfctrs: the reserved slots at
`SYSDEV_*CACHE` regs 14-15 will likely become `LINE_FILLS` and
`WRITEBACKS` post-phase-2, directly answering "did WA's read-fill
cost pay back via intra-line locality."

gen2 note: the L1↔L2 interface is designed to not foreclose this — it
stays neutral about L2's write policy (the arbiter is L1-facing; WB/WA
are L2-internal and L2↔memory concerns). See the write-policy-neutrality
section of `doc/internals/penumbra2/memory-interface.md`.

## Hardware: L2 write buffer (latency-hiding alternative)

A smaller, orthogonal option to WB phase 2: a 1-4 word write
buffer at the L2's downstream port.  Hides write latency (CPU
stops stalling on the SDRAM round-trip) without reducing SDRAM
write traffic.

For the single-master system (nothing else contending for SDRAM
bandwidth) latency hiding and traffic elimination are nearly
equivalent in CPU-visible cost.  The write buffer is materially
cheaper to implement (~30-50 lines vs ~250-300 for WB+WA), and
the snoop complexity normally associated with write buffers is
handled for free by WT-WnA's byte-en update — L2's cached copy is
fresh as soon as the buffer accepts the write, so subsequent
L1 read-misses to that line return correct data from L2's cache
without needing to consult the buffer.

When to pick this over phase 2:

- Lower complexity budget / faster-landing intermediate
- Nothing shares the bus with the CPU (no copy engine on the
  master's side)
- Want to confirm the CPU-side latency-hiding hypothesis cleanly
  before committing to the larger WB engineering effort

When phase 2 is required instead:

- A copy engine on the master's side needs SDRAM bandwidth (CPU
  writes starve it)
- Sustained write rate exceeds SDRAM drain rate (buffer fills up,
  CPU stalls again)
- Power/thermal sensitivity to SDRAM utilisation

They compose: WB phase 2 + write buffer (= phase 3 in the L2 doc)
is the maximally aggressive design.

gen2 note: `doc/internals/penumbra2/memory-interface.md` keeps writes
a distinct single-beat shape so a buffer drops in cleanly, and leans
its placement *after* L2 for exactly the byte-en-update reason above;
it lists the buffer as a deferred fill-speed direction gated on gen2
measurements.

## Hardware: read-miss fills pay 1-2 unnecessary cycles re-classifying

Both L1 (`cache_vipt.sv`) and L2 (`l2_cache.sv`) caches complete a
read miss by transitioning `S_FILL → S_IDLE` and then waiting for the
CPU's still-asserted request to be re-latched and re-classified as a
hit on the just-installed line.  That re-classification costs 1 extra
cycle at L1 (single-stage classify) and 2 extra cycles at L2
(stage 0 + stage 1) on every read miss.

Logically the cache could short-circuit this: at the cycle the last
word of the fill arrives, all words of the line are accessible
(those captured previously from BRAM, the last one from
`i_mem_rdata`), and the CPU's requested word offset is known from the
stalled address.  Driving `o_rdata` from the appropriate source and
`o_busy=0` on that cycle exits the CPU's STALL one or two cycles
earlier.

Two layered variants on the same idea:

1. **Skip re-classification.**  Serve the requested word on the
   last-word-captured cycle.  Saves 1-2 cycles per miss regardless
   of which word in the line was requested.
2. **Early restart.**  Serve the requested word the moment *it*
   arrives during `S_FILL` (not waiting for the rest of the line —
   the remaining words finish into BRAM in background).  Burst
   order does not help it: where the line stack is built (Level 2 in
   `doc/internals/sdram-optimization.md`) every word lands at once,
   so early restart would save the bus walk of words 1–3 and the L2
   re-serve, whichever word was requested — see "partial-line
   delivery for fills" under Ruled out.

Per-cycle savings are modest against the ~30-cycle SDRAM round-trip
on the single-issue microcoded CPU we run today.  For the planned
pipelined Penumbra/2 each saved cycle is one fewer dependent-chain
stall, and the higher target clock makes each cycle more expensive
in wall time, so the optimization is higher-leverage there than the
bare cycle counts suggest now.

**Update — Penumbra/2 design settled.** gen2's L1↔L2 interface takes
an *atomic full-line* fill (Decision 14 in
`doc/internals/penumbra2/design-decisions.md`; spec in
`doc/internals/penumbra2/memory-interface.md`), which splits the two
variants above by gen2-compatibility. **Variant 1 (skip
re-classification)** keeps the whole line present, so it is compatible
— gen2 may take it. **Variant 2 (early restart)** is deferred to the
non-blocking phase: serving a word before the line completes punctures
the transaction-atomicity invariant the gen2 arbiter and fill path
rely on (it forces per-word presence, hit-under-fill stalls, and
fill-vs-store / fill-vs-flush handling). So for gen2, early restart is
explicitly not the win the preceding paragraph assumed.

The L1-side win is more impactful than the raw cycle counts imply,
in a way that scales with L2 hit rate.  An L1 miss that hits in L2
costs roughly `4 words × ~3 cycles` ≈ 12 cycles of fill + 1 cycle
of re-classification — so the re-class is around 8% of the miss
cost on that path.  An L1 miss that *also* misses L2 (full SDRAM
round-trip) costs ~120+ cycles, and the same 1 cycle is less than
1% of it.  Hot kernel paths sit on the L1→L2-hit path
(syscall I-fetch is the canonical case where L2 lands ~2× wins),
so the bulk of L1 misses in real workloads are exactly where this
optimization saves the largest fraction.

Side effect on the perfctr work landed alongside this entry: both
L1's `state_was_fill` guard and L2's `fill_reserve_pending` flop
exist specifically because the post-fill re-serve generates a
phantom hit event today.  Once early-restart lands the re-serve
goes away on both caches and both predicates collapse to
`<valid> && <s1_re> && hit` with no suppression.

## Hardware: what limits each top, measured with four audio channels

Sweeping all three board tops with the audio device at four channels,
per-channel rate and gain, before the device was given a floorplan
region:

```
            clk                       60 MHz domain        all domains
gen1     mean 25.57  7/10  (25)    mean 68.98 10/10       9/10
gen2     mean 35.08  1/10  (37.5)  mean 67.95 10/10       1/10
gen2.5   mean 35.32  0/10  (37.5)  mean 65.47 10/10       0/10
```

Seed-to-seed spread swamps any single change at this margin: one seed
moved 3.2 MHz on the CPU clock after an edit that removed 1% of the
design's cells and touched nothing on its path. A ten-seed sweep here
resolves nothing below a few MHz, so what it answers is "is there a
usable seed, and is it the pinned one" rather than "did this change
help".

**No critical path on any top contains an audio cell on the CPU
clock**, which is the domain that binds. The 60 MHz domain is a
different matter — on some seeds its critical path runs
`u_audio.chan_en_a2` through the mixer's phase adder — but that domain
clears with 12-18% in hand, so it is not what costs the design margin.

What the device costs is die. It is roughly 1600 LUT4 and 1600
flip-flops with eight block RAMs and four multipliers for the register
and mixing tiers, plus the ladder output stage on top; measured as
placed cells that is some 4175 of the design's 38371, near 11%. The
placer spends that occupancy on whichever path was already closest to
its constraint. What binds is structural and predates the device:

- gen1's 60 MHz domain is the USB receive chain on all ten seeds,
  every one ending at the host controller's CDC buffer
  ([above](#hardware-the-usb-receive-path-is-a-placement-lottery)).
- gen1's CPU clock is the microcode ROM cone — ROM to ROM, or address
  to ROM, on six of ten.
- gen2.5's CPU clock is `EX.o_op_class` to `MMU.o_b_fault_status` on
  five of ten, at 49–55 hops. The three seeds that clear run a
  different and shorter family instead — D-cache tag to the ID stage,
  35–37 hops — which lands at 26.2–26.4 ns against a 26.67 ns budget.
  Even the winners hold about 1%.

Every one of these is routing, not logic: 3.8–6.1 ns of logic against
21–24 ns of routing, so 80–85% wire. That is the same signature as the
USB chain, and it says the same thing — these cones span module
boundaries and the placer cannot keep them local. Adding pipeline
stages inside a stage would not obviously help; what would is making
the cone shorter in *distance*, which means either fewer boundaries
crossed or a registered boundary at one of the crossings.

### The CPU clock's cone is long, not deep — and the fix is a region

Measured per hop on the critical path, across ten seeds on gen2.5:

```
  domain            hops   ns/hop   tiles/hop   ns/tile
  clk                544     1.22      14.23      0.272
  clk_60mhz          144     0.76       4.60      0.287
  clk_sdram           80     0.68       1.50      0.440
  clk_video_pixel    178     0.64       1.35      0.534
```

The CPU clock's hops travel three times further than the next domain
and ten times further than the SDRAM one, at the *best* nanoseconds per
tile of the four — long hops win faster routing resources. So the cone
is not detouring inefficiently; consecutive cells in it are simply
placed far apart, because the band they occupy is full. In columns
X75-X89 the core had some 3000 cells, the L1s 1500, and the audio
device 1800: a third of the sites in that band went to a device with
12-18% of margin on its own domains, sitting among cones that had none.

The placer cannot know that. It minimises total wirelength, every
module reaches every other through the bus, so it clumps everything
into one band and leaves X2-X47 empty — 46 columns of fabric a small
design does use, the audio bring-up top placing 2080 of its 2272 logic
cells there. A region constraint is the only way to state which module
can afford the distance.

Constraining the audio device's output-clock side — mixer, queues, and
the synchroniser stages feeding them — to X24-X50 moves every one of
its cells out of X72-X95 and drops the densest band from 4633 cells to
3490. Against the same netlist and the same ten seeds, with gen1 as an
unintentional control because the floorplan is not wired to that top
and its numbers did not move at all:

```
            clk                       60 MHz domain        all domains
gen1     mean 25.57  7/10  (25)    mean 68.98 10/10       9/10   (unchanged)
gen2     mean 36.91  5/10  (37.5)  mean 67.14  8/10       3/10
gen2.5   mean 35.99  4/10  (37.5)  mean 69.13 10/10       4/10
```

gen2.5 goes from no usable seed to four. Note what moved: its mean rose
only 0.67 MHz, well inside the noise, while its spread doubled — and
because the constraint sits above the mean, a wider distribution is what
puts seeds over the line. gen2's mean rose 1.83 MHz, about two standard
errors. The device's own domain gains too, its hops falling from 12.0
tiles to 5.4 once its cells are clustered.

What does *not* follow is a clean causal account. The winning placement
reaches the same `EX.o_op_class` to `MMU.o_b_fault_status` endpoints
through fewer hops rather than shorter ones, so the eviction changes
which path the placer finds rather than compressing the one it had.

### The SDRAM region's rationale is stale; its effect is not

The floorplan script describes a corridor between the core's right edge
at X113 and the SDRAM pads, which no longer exists — the core reaches
X110 and L2 cells now run to X124. A stale rationale is not evidence of
a stale constraint, though, and the two separate cleanly under a
control: placing without any floorplan puts `clk_sdram` at 104 MHz
against its 100 MHz constraint, where the region holds it at 123. It
is worth some 18 MHz and stays. Rewrite the reasoning, not the bounds.

### Sequencing bounds cycles, not depth

The mixing datapath is walked one channel at a time because a frame
period is some 1250 output clocks and the walk is 34. The first
version spent that budget on the walk and then did the whole of one
channel's work in a single cycle: channel mux, gain multiply,
accumulate, master multiply and saturation. That measured 30.14 ns on
a 16.67 ns budget — 117 hops, two cascaded DSP multiplies — and put
the audio domain at half its required frequency on every seed.

Splitting it into five stages, with the sample and gain registered
ahead of each multiply so the DSPs see registers on both sides, took
the domain from 29.95–34.48 MHz to 62.91–72.54 and moved the critical
path back to the USB chain. Utilisation did not move: the pipeline
registers replaced combinational logic rather than adding to it.

The lesson generalises to anything sequenced over a slow tick. A large
cycle budget is an argument about *how many* cycles are available and
none at all about how deep one may be; a shared datapath with that
much slack should be pipelined as far as it will go, because the
registers are free.

## Hardware: the USB receive path is a placement lottery

`clk_usb`'s critical path is an unregistered chain running the length of
the receive stack — nine module boundaries in one clock:

```
usb_phy.rstate_q → ka_q → drive_en → squelch
  → oversample.i_line → rx_framing.i_bit_en → unstuff.i_en
  → deserialize.i_valid → pkt_rx.byte_arrives → payload_byte
  → active_q → usbhc.mac_we → usbhc_cdc.buf_mem address
```

31 hops, 16.84 ns against a 16.67 ns budget on the seed that exposed
it. About 3.4 ns of that is logic (14 LUTs at 0.24 ns) and 13.4 ns is
routing, and the final two hops into the CDC buffer's address port are
4.20 ns on their own — a quarter of the budget in one wire.

The delay is therefore *distance*, not contention: die utilisation is
27% of LUTs, so nothing is congested. Each hop lands wherever its
module was placed, which makes the path's length a property of
placement rather than of logic, and placement is what a seed
randomises. Measured spread across ten seeds on gen2 without the audio
device: 64.8 to 76.9 MHz, mean 72.0.

That spread is the real problem. A domain whose fmax varies 19% with
the seed passes on luck, and the luck runs out on the next change:
adding the audio output stage — some 250 LUTs in that domain, 3.7% more
logic overall — moved the mean down 7.6% on gen2 and 6.8% on gen2.5,
taking gen2 from ten seeds clearing 60 MHz to nine and gen2.5 to seven.

Worth fixing rather than re-pinning around, for two reasons. The chain
is enable and valid signals threaded through the receive pipeline, so
registering it is a local change to signals that already have a natural
cycle boundary, not a protocol change. And `usbhc` is a bus device: a
new core generation reuses it verbatim, so the fragility travels with
it rather than being retired alongside the core it was measured on.

### Method note: attribute against a swept baseline

The failure first looked like the audio device breaking USB timing. It
was not diagnosable either way until the pre-change design was swept
across the same ten seeds, because the pinned seed showed one sample of
a wide distribution. The baseline is what turned "a seed failed" into
"the mean moved 5.5 MHz and the passing-seed count went from ten to
nine". Sweep before attributing.

### gen2.5's own ceiling is elsewhere

The same campaign measured why gen2.5 fell from seven usable seeds to
two, and it is not the audio device: its `clk` critical path runs
EX → MEM → MMU → TLB → permission check → fault status, 51 hops and
26.0 ns against a 26.67 ns budget, with the permission check alone
8.85 ns over 18 hops. Audio appears nowhere on it. gen2.5's baseline
`clk` mean is 38.2 MHz against a 37.5 constraint — 1.8% of margin on
seven seeds of ten — so the variant was already at its ceiling and the
audio device consumed what was left.

The chain is unregistered because a fault must be known in the same
cycle as the access, to squash and trap. That is an architectural
choice rather than an oversight, and a design resolving faults a cycle
later would not have this shape — worth deciding deliberately in a
future core rather than inheriting by default.

## Hardware: main TLB geometry -- associativity against fmax

The main TLB is 32 sets of 2 ways with the victim chosen from a
`CPU_CYCLES` bit.  Replaying a real access stream through shadow
models says the shape, not the capacity, is where the misses are: 16
sets of 4 ways at the *same* 64 entries takes 62% fewer, and an LRU
victim at the existing shape takes 22% fewer.  The models and the full
table are in [kernel.md](kernel.md), "Kernel: count TLB refills".

By measured cost this is the largest item on the list.  The refill
handler is 39% of every instruction executed on a shell and filesystem
workload, and nothing on the software side reaches it: link-time
placement of hot pages is bounded at low single digits ("What pinning
can reach", same entry), and a third to a half of kernel refills fall
on runtime allocations no arrangement can touch.

Capacity is not on the table on gen2.5: neither a larger L1 nor a
larger TLB fits the ECP5, so re-shaping the 64 entries is the only
form this can take there, and anything else waits for gen3.

What is unknown is whether the machine can afford even that.  The TLB
sits on the address path of every access, and the fetch/TLB path is
already the limiter on more than one top, so the honest experiment is
to build the main TLB 16x4 and read fmax from the full board top
rather than the bare-core probe.  If it costs clock it is not a lever,
and that is the expected answer.  A seed sweep is needed either way,
since one seed says nothing about placement.

## Hardware: memories must map the same way under yosys and Vivado

One RTL source feeds both FPGA flows, so every memory declares its
storage and is written in a form both yosys and Vivado map alike
(coding standards, "Toolchain Compatibility"); yosys fails a build
that breaks the declaration and the Vivado signoff gate does the
same. The gen2 caches, the gate and the declarations landed in
2920dbc, fe677f1, 92de501 and 08ed166. What is left:

- The SDRAM CDC's slot arrays stay undeclared on purpose while the
  ECP5-side crossing is being reworked; `slot_we` maps to flip-flops
  under yosys and LUT RAM under Vivado.

## Hardware: gen3 sysreg accesses act on their target at a fixed cycle

A gen3 `WRSYS` lands in its device on the cycle it commits from EX,
and an `RDSYS` takes its response from the device in MEM2. Neither
lets the target say when it can take the access, so whatever the
target is doing in that cycle has to be reconciled in logic.

The main-TLB install shows the cost. A `TLB_PTE` commit writes both
copies of `penumbra3_tlb_store` while their read ports keep reading;
the I-copy is serving the fetches behind the `WRSYS`. yosys cannot
prove that the write and a read never meet on one set, so it keeps
the RTL's read-first result with a read-during-write bypass: the
write delayed a cycle through flip-flops, and a compare and mux on
the block RAM outputs, inside the verdict cone after the EBR
(`doc/internals/penumbra3/mem-stage.md`, "The MEM2 verdict cone").
`synth_ecp5` puts it at 134 LUTs and 137 flip-flops per copy: 313
LUTs and 203 flip-flops against 179 and 66 without it.

`penumbra3_tlb_store` now gates each copy's read off in the install
cycle, and yosys drops the bypass. That is free only because no
overlapping lookup is consumed today: the `WRSYS` commits with the
pipe drained behind it, and the resync re-fetches its successor the
next cycle, discarding every lookup the install overlapped. It stays
free only while TLB installs serialize, and the refill path wants them
not to (`doc/internals/penumbra3/timing-outline.md`, "Multi-cycle and
serializing instructions (div/mul, WRSYS, ERET)"). A non-serializing
install makes the I-copy's overlapping lookup live, and that lookup
then has to wait a cycle rather than read the block RAM mid-write.

`RDSYS` has the mirror problem: the response is taken from the
device in a fixed cycle, so a device whose status is not already
registered puts its status logic in the core's result path.

Open: a sysreg access the target paces. The target accepts a write
in a cycle of its choosing and answers a read from a register once
it is ready, so an install lands in a cycle with no lookup launched
and a status read leaves the device registered. This fits the gen3
rule that rare and slow operations pay cycles, never critical-path
logic.

## Hardware: the gen3 L1's valid bits are most of its logic

`penumbra3_l1_store` keeps its 512 valid bits in flops, as gen2's L1
does, so that reset and `INVAL_ALL` clear them in one cycle. Synthesized
for the ECP5 on its own (yosys, out of context), the store is 1719 LUT4
with them and 509 without: about 1200 LUT4 and 512 flops for the write
decode, the clear and two 256:1 read muxes. The store bypass beside
them is about 80 LUT4. Each L1 pays this, and the PLRU bits in
`penumbra3_l1` are a second flop array of the same shape, not measured
apart.

Open: the only timing evidence is the P0.5 probe, which holds both L1s
whole and clears 37.5 MHz with 13–19 % slack over ten seeds
(`doc/internals/penumbra3/phase0-probes.md`, "P0.5 -- physical-tag
compare behind the TLB verdict"). That covers the cones through the L1,
not what the arrays cost a full machine's routing. No replacement has
been looked at.

## Hardware: a gen3 load behind a fill into its set waits a cycle

The L1 shows an installed line to a lookup it already holds two cycles
after the install, and says so in the install's cycle (`o_extra_cycle`
in `penumbra3_l1`). The load or store held in MEM2 behind a miss holds
such a lookup, so when the fill went into its set the launch side
resumes a cycle after the miss completes rather than with it. The next
word of the line just filled is the common case. Against an L2-hit fill
estimated at about 5 cycles that is one cycle in six, and each hop on
the fill path is worth several percent of cycles at the measured fill
rates (`doc/internals/penumbra3/memory-performance.md`, "The line-wide
transfer").

The cycle goes if the held lookup sees the install at once. Its valid
bit and word can be given it off the hit path: the valid bit set as the
output flop reads it, the word as a third input of the store bypass's
mux. The tag is the cost. A mux after the tag block RAM sits in front of
the compare, on a path that binds today (P0.5's worst paths start at
a TLB or an L1 tag block RAM, `doc/internals/penumbra3/phase0-probes.md`,
"P0.5 -- physical-tag compare behind the TLB verdict"). The
alternative is the block RAM forwarding it itself: the install written
through the lookup's own read port in write-through mode when it lands
in that set, the second port taking other sets. A write-through port
needs the true dual-port mode, at most 18 bits a port on the ECP5
(yosys's `DP16KD` map) and on Artix-7, so the 20-bit tag takes two block
RAMs a way instead of one: four more for the two L1s.

Open: how often a fill has a load or store of its set right behind it,
counted on a running core or by the ISS cache model, and the
write-through form's mapping under yosys and Vivado with a sweep on
both sides of it.

## Hardware: a gen3 drain-commit's wait sits on the issue stall

ERET, WRSYS, EI and DI commit from EX, so EX holds one until nothing
older is unfinished. That test is an OR over EX/MEM1, MEM2, WB, a
pending access and its completion. It feeds `ex_local_stall` and so
ID's stall, which every instruction's issue reads, though only these
four need it. No measurement says what it costs: gen3 has no full top
to read, and a path's place on a probe attributes nothing.

Two other shapes avoid the gate. Neither is built.

- **A registered `drained`.** While a drain-commit waits in EX nothing
  enters behind it, so the back end only empties. A flop sampling the
  gate with the drain-commit already in EX therefore cannot go stale.
  It costs a cycle per drain-commit and no new flush logic.
- **Commit at WB.** The instruction flows down the pipe like any other
  and commits as the oldest, so there is nothing to wait for. Whatever
  entered behind it ran on the old state, so the commit flushes it,
  reusing the fault commit's flush and adding a redirect. Each one then
  costs a refetch where EX waits only for the back end, and they are
  not rare: `_real_miss_handler` runs at least three WRSYS and an ERET
  per TLB refill.

Open: the gate's weight in a full-top seed sweep, with and without it,
and how often real workloads run these four against what a refetch
costs. The test any shape has to keep is the `dc_*` group in
`tb_penumbra3_spine`.

## Hardware: an L2 INVAL_ALL write is dropped or drops a strobe

`l2_cache.sv` takes a `WRSYS` to `CACHE_INVAL_ALL` as a one-cycle
pulse, acted on only in `S_IDLE` with no request in stage 1. One
arriving during a line fill or behind a stage-1 read is dropped
without a trace. One arriving while a pass-through access waits on
the bus enters `S_INVAL_ALL`, which stops driving that access's
strobe before its ack, against the bus rule that the master holds
strobe and address through the ack (`doc/hardware/bus-protocol.md`).
Found by reading the code, not observed: nothing issues an L2
INVAL_ALL at runtime today. The `CTRL` enable had the same race;
`hw: apply an L2 enable only with no access in flight` fixed that
half.

Open: hold the request pending and start the walk at the first edge
ending a cycle with no access presented, as the enable does
(`doc/internals/l2-cache.md`, "When a CTRL write takes effect").
Regression: an `l2_cache` unit test that issues INVAL_ALL during a
fill and during a pass-through read.

## Hardware: MUL and DIV iterate over every bit, whatever the operands

The divmul unit walks all 32 bits of `Rd` for a multiply and produces
all 32 quotient bits for a divide
([divmul.md](../internals/divmul.md)): 10 cycles for a multiply at
four bits a step and 34 for a divide, whatever the values.  Counted
2026-10-08 by the ISS's operand census (`+opstats`, with
`+core=penumbra2_5` for the step width) over `pbench kernel`, Doom
`demo3` and the bare-metal CoreMark (the whole run, both seed sets),
the operands seldom need that.  Steps are those up to an operand's
highest set bit, on magnitudes, as the unit works:

| | `pbench kernel` | Doom `demo3` | CoreMark |
|---|---|---|---|
| `MUL`, `MULU` per 1000 instructions | 1.4 | 3.0 | 24 |
| Steps needed of 8, walking `Rd` as built | 6.9 | 3.8 | 2.2 |
| … walking `Rs` | 5.7 | 4.1 | 1.9 |
| … walking the smaller operand | 5.6 | 2.6 | 1.7 |
| `DIV`, `DIVU` per 1000 instructions | under 0.01 | 0.76 | under 0.01 |
| Steps needed of 32, past the dividend's leading zeros | — | 28.2 | — |
| … with the dividend aligned to the divisor | — | 12.8 | — |

- **CoreMark is where the multiply costs most:** 24 per 1000
  instructions, about 240 cycles per 1000 as built.  Its matrix
  kernels multiply 16-bit elements, so a multiply that stops at `Rd`'s
  highest set bit saves 140 of them, and 153 walking the smaller
  operand.
- **On Doom, stopping early would halve what the two cost.**  They
  take about 55 cycles per 1000 instructions there.  A multiply that stops
  at `Rd`'s highest set bit saves 13 of them, 16 walking the smaller
  operand; a divide aligned to its divisor saves 15.
- **Skipping the dividend's leading zeros alone saves 3.**  Doom's
  dividends mostly fill the word: each wall column divides
  `0xffffffff` by a scale.  Aligned to the divisor, two thirds of its
  divides need 16 steps or fewer, and 8 % need none: the dividend is
  the smaller.
- **The kernel paths gain 2–3 cycles per 1000.**
- **The audio mixer is not counted.**  It multiplies an 8-bit sample
  by a volume of at most 64, two steps of the eight, and the multiply
  is 10 of its ~40 cycles per voice-sample (`workloads.md`, "Compiler:
  hot-loop code review — software audio mixing").

Open:

- **The multiply shifts its product into place a step at a time,** so
  one that stops early has the rest of the shift still to do: at once,
  or by a walk that keeps the product in place.
- **Aligning a divide** takes the highest set bit of both operands and
  the dividend's upper bits preloaded as the remainder.  Whether that
  is for the FPGA cores alone, as more than one multiplier bit a step
  is, or within the discrete build's budget.
- **Timing that depends on the operands** changes what
  `doc/system/instruction-timing.md` states ("`MUL` and `DIV` take the
  same time whatever their operands") and the single latencies the
  compiler's scheduling models carry (`compiler-pipeline.md`,
  "Compiler: per-generation tuning — models and their consumers").
- **Count the mixer,** and a workload that divides small numbers.
- **Count a fixed-point audio decoder.**  Integer MP3 or Vorbis
  decoding is about what a core of gen4's class can carry, and its
  filter taps are each a 32-bit coefficient times a 32-bit sample with
  the high half kept.  If those operands fill the word as expected,
  stopping early saves least where multiplies are most of the work,
  and more multiplier bits a step is the lever instead.  No such
  decoder is built for Penumbra; none of this is measured.

## Ruled out

- **Partial-line delivery for fills.**  Critical-word-first burst
  order, and the BL=8 line crossing the CDC in halves or single
  words, were analysed and declined.  Fills are atomic, so only the
  last word counts, and the registered-ack bus paces words at two
  cycles while the SDRAM produces one every 0.75 (37.5 MHz): an early
  piece saves only as much as word 0 lands ahead of the whole line,
  about 1.5 cycles per fill for halves and 2.25 for single words, of
  a ~47-cycle gen2.5 L2 read miss.  Word order never shows.  A split
  transfer also acks word 0 with the rest in flight, so an uncached
  read then a write to the same line can buffer pre-write data —
  the discard-on-arrival tracking behind the speculative adapter's
  past races.  Analysis in
  `doc/internals/penumbra3/memory-performance.md` ("SDRAM side:
  whole-line fetch").
- **Synchronous CPU/SDRAM clocking.**  Clocks from one PLL at a
  whole-number ratio would replace the async CDC's 4–5 cycles per L2
  miss with ~1.5.  Declined: with USB's 60 MHz on the same PLL and the
  10 MHz phase-detector floor, the only legal VCO is 600 MHz, whose
  integer pairs cost the CPU clock or most of the saving; and nextpnr
  reports a path between PLL clocks as a delay only (no slack, no
  failure, no placement weight, `--ignore-rel-clk` a no-op), so the
  crossing would go unverified.  Worth ~1–1.5 % of Doom and the kernel
  paths.  Analysis in `doc/internals/penumbra3/memory-performance.md`
  ("Synchronous CPU/SDRAM clocking — ruled out").
- **Read/write exclusivity in gen2.5's block RAMs.**  Stating in the
  RTL that the L1s', the L2's and the SPI/UART FIFOs' reads never meet
  their writes lets yosys drop its read-during-write emulation: 434–553
  LUTs and 882 flip-flops on the gen2.5 ULX3S top.  Over seeds 0–29 on
  4e91777 the CPU clock fell from a 36.84 MHz mean, with 8 of 29 seeds
  closing, to 36.38 MHz and 4 of 30 with the L1s gated on the write
  side, and to 36.11 MHz and 2 of 30 gated on the read side.  The
  emulation delays each write through flip-flops, and in the caches
  those flops were registering the write enables that depend on the
  tag compare; without them the L2's tag, hit and array write-enable
  path became the worst on up to 9 of 30 seeds.  A design that plans
  its writes around the rule, as gen3's does
  (`doc/internals/penumbra3/timing-outline.md`, "4. Caches"), registers
  them on purpose; retrofitting the rule onto gen2.5 did not.
- **Flattening gen2.5's core hierarchy.**  Dropping `keep_hierarchy`
  from IF1, IF2, the spine, the MEM stage, the perfctr and the four
  translate-cone modules lets ABC merge logic across their boundaries:
  356 fewer LUTs and 160 fewer flip-flops, and the median CPU-clock
  critical path fell from 24 LUT hops to 19.  Routing per hop rose
  from 0.84 to 1.15 ns, leaving the ~20.4 ns of routing unchanged, and
  over seeds 0–29 on 4e91777 with 458fd30's Z-flag tree the CPU clock
  averaged 37.17 MHz with 13 seeds closing, against 37.73 MHz and 18
  without the change.  A merged LUT draws its inputs from modules
  placed apart, so the saved levels come back as longer wires.
- **Hit under miss in the L2.**  Serving an L2 hit while an L2 miss
  waits on SDRAM, or overlapping a fetch fill and a data fill, was
  analysed and declined.  A core that holds the pipe on a miss uses at
  most the shorter of two overlapped fills, and its in-flight
  instructions hide most of that: the one case that saves anything, a
  data fill finished under a fetch fill from SDRAM, is worth ~0.2–0.7
  cycles per 1000 instructions, and contention of any kind at most
  11–25.  It pays only with stall-on-use loads or out-of-order issue,
  which `doc/internals/penumbra3/memory-completion.md` rules out.
  Analysis in `doc/internals/penumbra3/memory-performance.md` ("Hit
  under miss in the L2 — ruled out").

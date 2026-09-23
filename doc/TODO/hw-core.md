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
  verdict + cache hit, sub-word extract, fault merge, the `load_complete` hold
  buffer; `load_complete` parks the full descriptor and `dtranslate`/`tlb_store`
  gained an `i_hold` read clock-enable for the freeze; composed
  mem1->dtranslate->mem2 test (`96da15b`)
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
  the freeze distribution (registered `load_pending` freezes the launch side +
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
them tied off today, so a load/divmul-use waits for the registered clear) and
the *load_pending-freeze / memory-path test* (the load path is wired but the
skeleton test drives no translate/cache/fill verdicts yet, so the load-miss
freeze is unexercised).

**Memory hierarchy** (the fork in progress):

- `penumbra3_translate` -- the side-neutral translate path (renamed from
  `dtranslate`; the MMU uses it twice), now exposing its registered set for
  sysreg read-back (`9fd2928`, `460f6d4`)
- `penumbra3_mmu` -- I+D translation unify: `translate` x2 over a shared
  coherent write stream + dual-port `tlb_pinned`, per-port bypass>pinned>main
  verdict, the device-0 sysreg interface (MMUCR / FADDR / FSTAT / TLB
  install+read-back), commit-time fault registers (`02f57e3`)

**Next, in order:** L1 caches (passive: index->tag/data + fill-install, no
embedded FSM -- miss orchestration is in `load_complete`) -> L2 (write-through)
-> fill sequencer -> arbiter (-> `bus_master`) -> `machine_penumbra3` +
`ulx3s_penumbra3_top`. **Open decision at the L1 step:** the store write-through
path is unbuilt -- `mem2_stage` writes the L1 copy (`o_dcache_we`) but issues no
write-through to L2/memory and does not block (stores retire via `mem2_commits`).
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
- The back-end stall is spine-distributed: MEM2's `load_complete` produces the
  registered `load_pending`, and the spine fans it back to freeze the *launch
  side* -- MEM1, the MEM1/MEM2 register, `dtranslate`/`tlb_store`/cache, and
  issue -- so the missing load and everything younger hold. It must NOT freeze
  the MEM2/WB output register: that drains older work to commit (the held load
  retires via the completion path, not MEM2/WB), and folding `load_pending` into
  its freeze re-presents an already-committed multi-cycle writeback (the divmul
  dual-register retire) and retires it twice. The MEM2/WB register's only
  back-pressure is the WB dual-write hold. Contract:
  [load-completion freeze scope](../internals/penumbra3/memory-completion.md#what-the-pipe-gate-freezes).
  A frozen MEM2 slot keeps its own verdict, and no combinational cache/TLB
  verdict ever reaches issue. Candidate spine assertion: `load_pending` never
  gates the MEM2/WB register.

**Deferred (resolve at the named task):**

- *Issue-release broadcasts* -- the operand-value forward network is settled
  (EX sources EX/MEM1, MEM1/MEM2, MEM2/WB; youngest-wins; write-first regfile
  for the four-deep distance; the MEM1 leg excludes loads/sysreg reads; MEM2
  exposes `o_fwd_*` for the load-use leg). What remains is a pure spine task:
  drive ID's `i_fwd*` to release a scoreboard-pending load / sysreg read at the
  cycle its value becomes forwardable -- gated on the registered `load_pending`,
  never a live verdict. ID owns only the match today.
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
  ~load_pending` (the drain-commit condition extended with `load_pending`) --
  then writes Rd/Rdh over two cycles through the completion write-port mux. The
  port is free **by the in-order single-outstanding invariant** (oldest => no
  older writer; younger blocked behind the held divmul), NOT by any cycle count
  -- so a slow uncached MMIO access ahead of the divmul simply makes it wait in
  EX, never write early or out of order. WB then never sees a divmul: the
  dual-write FSM and `wb_local_stall` both delete, and back-pressure reduces to
  the `load_pending` fan-out plus the one-hop EX->ID stall. Land as its own
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

The per-store figure predates `doc/internals/instruction-timing.md`,
which measures a write-through `STW` at 8 cycles on penumbra1 and 5
on penumbra2_5 in steady state; re-measure before sizing the win on
it.

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
  coherence (DMA-out preparation, etc.)
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

For the current single-master system (no DMA contending for SDRAM
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
- Single-master system stays single-master
- Want to confirm the CPU-side latency-hiding hypothesis cleanly
  before committing to the larger WB engineering effort

When phase 2 is required instead:

- Future DMA needs SDRAM bandwidth (writes from CPU starve other
  masters)
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
   order does not help it: with the BL=8 line buffer (Level 2 in
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

What is unknown is whether the machine can afford it.  The TLB sits on
the address path of every access, and the fetch/TLB path is already
the limiter on more than one top, so the honest experiment is to build
the main TLB 16x4 and read fmax from the full board top rather than
the bare-core probe.  If it costs clock it is not a lever, and that is
the expected answer.  A seed sweep is needed either way, since one
seed says nothing about placement.

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

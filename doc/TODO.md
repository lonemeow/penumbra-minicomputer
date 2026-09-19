# Penumbra -- TODO

Outstanding work and roadmap items, plus durable findings from
completed investigations.

## Hardware: gen2 SDRAM CDC request-handshake skew (RESOLVED)

gen2 Dhrystone hung silently before printing. The proximate symptom was
an uncached read returning a stale word — a vector-table fetch dispatched
to an unmapped ROM page and re-faulted into the same vector forever. Made
deterministic by a read-after-write checker in `sdram_sim`, which caught
a read of `0xd000` returning `memory[0x8114]` instead of the installed
value.

Root cause in `sdram_cdc`: request acceptance used a *registered*
`o_sys_req_ready`. The slot was latched on cycle T (when `!sys_full &&
i_sys_req_valid`), but ready pulsed on T+1, with a `just_accepted`
deadzone suppressing a re-latch. The speculative adapter drives its
request combinationally and treats `i_req_ready` as "my current request
was accepted." When the master changes what it drives across that
one-cycle gap — a speculative `addr+4` push one cycle, then a real
(mispredicted) push the next — the T+1 acknowledgment is for the T latch
(the spec), but the adapter attributes it to the real push. The spec is
physically stored yet tagged REAL, so its response (the open-row word at
the spec address) is delivered to the real read, and the real address is
never latched into any slot at all. gen1's sequential fills never change
the driven request across the gap, so they never trip it; gen2's
mispredicting uncached/jump traffic does.

Fix: make acceptance a single-cycle valid/ready transfer.
`o_sys_req_ready` is combinational (`!sys_full`), the slot latches on the
same edge, and the deadzone is gone. Latch and acknowledgment are one
atomic event, so the master's per-accept bookkeeping always names the
request actually stored. Speculation stays enabled (no latency-hiding
lost); removing the per-accept deadzone is in fact slightly faster.

Removing the deadzone exposed a second issue: `sys_full` counted slot
occupancy via the SD read pointer (`sd_rptr_bin_synced`, freed when the
SD side reads a slot), but the master's depth-2 response/tag tracking
drains only at response delivery (`sys_rptr_bin`, which lags) — so the
master could issue a third request into its depth-2 tracking. `sys_full`
now counts `sys_wptr_bin - sys_rptr_bin` (sys-local, matching the tag
lifetime); an `outstanding_sys <= 2` assertion guards it.

Regression net:
- A permanent, always-on read-after-write consistency checker in
  `sdram_sim.sv` (windowed to the low 2 MiB) `$fatal`s at the cycle and
  address of any stale read under every gen2 sim, so this class of bug
  reports itself instead of being chased.
- `tb_sdram_cdc` pins the handshake contract directly (single,
  back-to-back, distinct-address, depth-2-in-flight), driven to the
  combinational valid/ready protocol.
- `tb_sdram_sim` (new full-stack stress) drives gapless fills + gapped
  jumps with its own shadow check. It exercises the whole stack but did
  *not* deterministically reproduce this phase-sensitive case; the
  permanent checker is the actual regression net.
- `isa/test_trap_store` was added along the way (a memory store inside a
  fault handler — a conformance gap nothing else covered).

gen1 note: `sdram_cdc` ships in the gen1 ULX3S bitstream. The handshake
path is entirely on the 25 MHz sys clock (not the fmax-critical fetch/TLB
cone), and gen1 was confirmed to synthesize and run with the change.

## Hardware: SDRAM adapter may deadlock on a non-back-to-back read stream (suspected)

Found while probing the gen2 path toward 50 MHz. A single-outstanding
register slice inserted on the L2 <-> external-bus boundary (in
`machine_penumbra2`, to cut the combinational `L2-addr -> bus-decode ->
busy -> L2` round-trip that limits gen2.5) turns the L2 line fill into four
*gapped* single reads — `re` drops between words, +2 cycles each. With that
slice in place execution ran ~174 instructions and then wedged:
`test_l2_ifetch` hung at cycle 1448 ("no program end within 2000000
cycles"). Single bypassed reads (boot, MMU off) and the normal
back-to-back fill both work fine — only the gapped-mid-fill stream
deadlocked.

Suspected cause in `sdram_bus_adapter.sv`'s speculative-prefetch FSM. Its
own header says a mispredict (next read != the speculated `addr+4`) is
handled by abandoning the in-flight spec and discarding its response — so a
*deadlock* on a gap is a robustness bug, not an inherent limit. Likely
culprits: the "in-flight hit, stall in BEGIN" case never getting its
consume when `re` goes low mid-spec, or the 1-bit tag FIFO desyncing when a
spec response is orphaned by the gap. This is the same family as the
RESOLVED CDC handshake-skew bug above (speculation interacting badly with a
changed/interrupted request stream), so the spec state machine deserves a
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
  [load-completion freeze scope](internals/penumbra3/memory-completion.md#what-the-pipe-gate-freezes).
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

## Kernel: block-device reads still go single-block

`pmci` handles CMD18/CMD25 multi-block natively (a per-block
FIFO-burst loop inside a single command envelope), but the block
device doesn't reach it:
`dd if=/dev/ld0 of=/dev/null bs=32k` issues 64 separate 512-byte
`bread()` calls per syscall (`DEV_BSIZE = 512`), so each turns into
a single-block read in `sdmmc_mem_read_block_subr`
(`c_datalen / c_blklen == 1` → `MMC_READ_BLOCK_SINGLE`).  CMD18
never engages on the block-device path.  This is why
`dd if=/dev/ld0` shows ~85 KB/s while
`dd if=/dev/rld0 of=/dev/null bs=32k` and FS-mediated reads see the
~190 KB/s CMD18 number.

Options if/when this matters:
1. Teach `ld_sdmmc_dobio` to coalesce adjacent `bread()` requests
   before forwarding to `sdmmc_mem_read_block`.  Touches the buf
   queue path; modest complexity.
2. Bump `DEV_BSIZE` for `ld(4)` so block-device reads come in
   larger chunks by default.  Affects every consumer of the block
   interface; broader blast radius.
3. Convert callers that care to use `/dev/rld0` (raw) instead.
   No code change, but documentation/convention only — easy to
   forget.

Most filesystem-mediated I/O already takes the larger-buffer FFS
path (16 KB FS blocks → 32-sector CMD18/CMD25), so the user-visible
impact is mostly for raw block-device tools (`dd if=/dev/ld0`,
`disklabel`, etc.).  Leave the block-device path single-block until
something on the read path notably matters.

## Kernel: IRQ-driven SPI completion deferred

The SPI FIFO data-phase plan originally included
`intr_establish_xname()` + `cv_wait` on XFER_DONE.  Implemented and
benchmarked: **3× slower**
than polled (85 KB/s → 28 KB/s).  Root cause is the cv_wait → IRQ →
cv_signal round-trip costing ~12 ms — more than 10× the 660 µs SPI
burst it's waiting for.  See also `pbench pipe_pingpong` (~19 ms for
2 context switches + 4 syscalls) and `pbench fork_exit` (~600 ms).

Polled busy-wait remains the right primitive for sub-ms device waits
on the current scheduler.  Revisit once one of:
- Context switch cost drops to ≪ 1 ms (would benefit fork, pipe, signals
  too — scratch SPRs landed in 02de545943e7 and shaved a few percent off
  trap-entry, but the cv_wait round-trip is still ~10 ms; the rest of
  the gap is trap_common / pmap_activate / copyin)
- CMD18 multi-block — landed.  Each `pmci_burst` now covers up to
  N × 660 µs of wire time on the FS-mediated path (16 KB FS blocks →
  32-sector CMD18), so re-measuring IRQ-driven completion against the
  new envelope is worth a fresh attempt

## Hardware: FPU

Add a floating-point unit to the ALU. Currently using soft-float.

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

## Hardware: Penumbra/2 perfctr stall attribution is mis-timed — STALL_FLUSH over-counts (RESOLVED)

Resolved by the bubble-cause-tag scheme on both gen2 and gen2.5. Each pipeline
bubble carries a `BCAUSE_*` tag set where it is injected and propagated ID→EX→MEM
→WB; the perfctr charges a non-retiring cycle to the carried tag at the commit
point instead of to the live per-stage stall signals, so a short stall (whose
bubble outlives its signal) and a long stall's latency tail land in their true
back-end bucket (LOAD/STORE/FUNIT/HAZARD) rather than the front-end residual. The
front-end split was *not* deferred after all: the "larger fetch-path change" the
diagnosis feared (threading tags through the elastic buffer) proved unnecessary —
an empty fetch slot is classified at the starved cycle by `i_fetch_busy`
(memory-bound → IFETCH, else FLUSH), which is correct by the counters' own
definitions since the buffer absorbs fetch bubbles and the right question is the
front end's live state, not a carried token. RDSYS (the read path for the
counters themselves) is charged to the residual, never a precise back-end bucket,
so the instrument cannot perturb what it measures. Verified by
`test_cpu_stall_hit_load` (a cache-hit load burst now moves STALL_LOAD ≥ N and
does not leak to FLUSH — the old code scored ~0) plus a per-cycle one-hot
partition assertion that runs through the whole conformance suite on both cores.
The original diagnosis is kept below for context.

The CPU stall-attribution counters charge a non-retiring cycle by the stall
signals asserted *that same cycle* (head-of-line, evaluated at the retire
point). But the bubble that blocks retirement at WB was injected upstream,
roughly a pipeline-depth of cycles earlier, by a stall that may already have
cleared. So the attribution is mis-timed: a stall shorter than the pipeline
depth is *fully* mis-attributed (its bubbles reach WB after its signal
deasserts), and a long stall loses its tail. The orphaned cycles fall into
`STALL_FLUSH`, which is the residual catch-all — "non-retiring, and no stall
signal asserted." So `STALL_FLUSH` is **not** a front-end / branch-flush
counter; it is inflated by the latency tails of every short back-end stall.

Measured with an RTL-sim probe (a temporary per-cycle classifier in the gen2.5
core that buckets every non-retiring cycle by pipeline state, alongside a
replica of the perfctr's residual logic), on the representative regime —
cached-fetch + cached-data `memcpy` through the MMU, the same access pattern
real `memcpy`/`pmap_copy_page` hits:

- Of ~18.5k cycles the perfctr called `flush`, only ~5.2k were genuine
  front-end fill (pipe empty: no instruction anywhere in EX/MEM/WB). The other
  ~16k were back-end-stall latency tails — an instruction sitting in MEM with a
  bubble passing through WB and no stall asserted that cycle.
- Cross-checks pinning the cause: *no* flush cycle had a live data request
  (`o_dmem_re`/`o_dmem_we` both low), so it is not a current memory access;
  flush *persisted* with cached fetch, so it is not fetch-stall drain; and an
  A/B with the `ldw`→`stw` dependency broken (identical memory traffic, no
  load-use hazard) left the residual essentially unchanged, so it is **not**
  load-use hazard (only ~256 cyc were — those correctly land in `STALL_HAZARD`).
  It is memory-access latency tails surfacing late.
- A memory-free, hazard-free, BTB-predicted loop showed pipe-empty ≈ 0, ruling
  out any structural front-end-throughput bubble.

Implication for the gen2.5 roadmap: the priority ordering ("flush is #1, ~2.5×
the next lever") was read off a mislabeled bucket. The genuine,
prediction-addressable front-end flush is small (~5k on `memcpy`), which is
exactly why BTFN, the RAS, and the BTB each moved `STALL_FLUSH` only a little —
the predictors work as designed; the counter misled. The dominant real-workload
CPI lever is the **back end** — operand forwarding (which removes the load-use
stall *and* its latency tail, the bulk of the bogus "flush") and the memory
system — not further branch prediction.

Fix: tag each pipeline bubble with the cause that injected it, carry the tag
*with the bubble* to WB, and charge the non-retiring cycle to the carried cause
— so attribution no longer depends on how far upstream, or how many cycles
earlier, the stall was. Back-end causes (load / store / funit / hazard) ride
bubble tokens that flow ID→EX→MEM→WB, so they tag cleanly; this is the dominant
measured mis-attribution and the right first increment. Front-end causes
(ifetch / flush) are decoupled by the elastic fetch buffer, which drops bubble
tokens — an empty buffer *is* the front-end bubble, with no token to carry a
cause — so attributing those correctly needs the buffer to carry tagged
bubbles, a larger fetch-path change; defer it behind the back-end chain. The
change is perfctr-observability-only (no pipeline-behaviour risk) and
discrete-logic-friendly (a few flops + muxes per stage). The diagnosis and the
encoding sketch (a `BCAUSE_*` field) were worked out but not landed; the BTB
that prompted this investigation sits on the `gen2.5-btb` branch, separate from
this fix.

## Hardware: Penumbra/2 taken branch under a MEM stall lost its link write (RESOLVED)

A `BL`/`JMP` resolving in EX while an older memory op stalled MEM was discarded
instead of committing — its link write (`R13`) was lost. Surfaced as Dhrystone
on the gen2 RTL "crashing": `bench_main`'s prologue (a run of write-through
stores hitting cold lines) stalled MEM with `bl bench_init` right behind it; the
BL took its branch (front end redirected to `bench_init`) but never retired, so
`R13` kept `bench_main`'s caller link. `bench_init`'s closing `jmp r13` returned
straight to `_start`'s program-end `break`, skipping the benchmark — Dhrystone
printed nothing and BREAK'd at the *normal* end PC, which is why it read as a
crash rather than a hang.

Root cause: `penumbra2_id_stage`'s ID/EX register ranked `i_bubble` above
`i_stall_in`. `ex_branch_taken` rides `i_bubble` to kill a taken branch's
wrong-path *successor* — correct only once the branch advances out of EX. Under
MEM back-pressure (`ex_stall`) the branch is pinned in EX, so the ID/EX register
still holds the branch itself, and the bubble cleared it. Fix: gate that one
term — `i_bubble = (ex_branch_taken & ~ex_stall) | wb_fault_commit | … ` — in
`penumbra2_spine`. `wb_fault_commit` stays ungated (a fault must kill younger
slots regardless of back-pressure); `eret_commit`/`wrsys_resync` fire only with
the pipe drained, so they never coincide with a stall.

Diagnosed with the gen2 RTL trace (now implemented in
`tb_penumbra2_interactive`: retire stream, drain-commit lines, branch-resolve
markers, and an opt-in per-cycle EX/MEM/WB occupancy window via
`+pipe_lo=/+pipe_hi=`). The occupancy view was decisive — it showed the BL in EX
with the older store still occupying MEM, then gone the next cycle without MEM
advancing: a resolve-then-squash a retire-only trace cannot show.

Coverage gap this exposed: leaf testbenches drive their own stalls
(`tb_penumbra2_mem_stage` richly, `ex_stage`/`id_stage`/`if*` lightly), but the
*integration* (`tb_penumbra2_spine`) drove `i_dmem_busy = 0` always — zero
coverage of a stall *interacting across stage boundaries*, which is the only
place this bug is visible. Closed by the `run_mem`/`tick_mem` model (a
configurable multi-cycle MEM latency, borrowed from `tb_penumbra2_mem_stage`)
and Test 5 (a BL pinned in EX by a stalling load). Still worth adding on the
same harness: drain-commit under a MEM stall, a RAW consumer waiting on a
producer stalled in MEM, a fault committing while a younger branch is resolved
under stall (verifies the `wb_fault_commit` override — the opposite of this
fix), and a divmul immediately followed by a branch.

## Hardware: Penumbra/2 SPR access path — complete

WRSYS drives a real sysreg write at the EX drain-commit
(`make test-prog CORE=penumbra2 PROG=test_syswrite`). Wiring it surfaced a shared decode bug:
WRSYS *and* WRSPR encode their value register in the Rd field (as the assembler
emits and gen1 reads), but gen2 decode read it from Rs (i.e. R0). Both were
corrected to read the Rd field.

The SPR access path is fully wired, in backend-ordered pieces:

- **1a (done):** RDSPR/WRSPR EPC/ESR + RDSPR SR. EPC/ESR are SPR-file-backed
  (not regfile entries) and commit at WB like any register write — the value
  rides ALU_PASS to wb_value and the WB SPR strobe (previously tied off in the
  spine) drives the SPR-file write, gated to EPC/ESR. RDSPR reads them as
  operand B; the scoreboard downstream-writer predicate now counts `spr_we`.
  RDSPR SR composes {committed S/I, flag-bypassed NZCV} in EX. WRSPR is **not**
  a drain-commit — value SPRs are plain stores, unlike WRSYS. Tests:
  `isa/test_wrspr_epc`, `isa/test_rdspr_sr` (pass on ISS/gen1/gen2).
- **1b (done):** WRSPR/RDSPR USP — routes to the regfile R14 bank (entry 14,
  the `cross_bank` any-mode access the regmap already raises). RDSPR USP worked
  after 1a (ordinary operand-B regfile read); 1b ORed the USP SPR strobe into
  the regfile write enable. Test: `isa/test_wrspr_usp`.
- **1c (done):** WRSPR/RDSPR SCRn — `penumbra2_scratch_file` (4×32, one WB
  write port, one combinational ID read port) backs the four scratch SPRs,
  which fall outside the 16-entry regfile so they need storage of their own.
  The spine gates the write to the SCRn range and muxes the readback onto the
  operand-B SPR path; the ID stage routes SCRn reads through that path (USP
  stays on the regfile read). Test: `isa/test_scratch_sprs`.

With all value-SPR writes wired, gen2 advertises the `wrspr` capability
(`RUNNER_PROVIDES_penumbra2`), which pulled the previously-skipped `wrspr`-
tagged conformance tests into the gen2 suite. Two then failed — `test_priv`
(user-mode privilege trap) and `test_usp` (user-mode USP banking) — because
`penumbra2_core` hardwired `core_supervisor = 1'b1`, a bring-up stub deferred
"until the SR.S write path lands." With ESR/ERET restore real, that landed: the
spine exports the committed `SR.S` (`o_sr_s`, mirroring `o_sr_i`) and the core
sources its single `core_supervisor` site from it, so decode-time privilege
checks, both MMU user bits, regfile USP/SSP banking, and the fetch-side user
bit all see real privilege. Using the committed value is safe for every stage
because privilege changes only via drain-commit ops (exception entry / ERET),
which flush younger instructions. Full gen2 conformance suite green (72/72, 10
skipped on absent peripherals: bus/irq/timer/machid/perfctr/uart).

The privilege threading places the committed `SR.S` flop on the fetch/MMU path
(the gen2 fmax-sensitive cone). First `make timing` of the full
`machine_penumbra2` probe (`BOARD=ulx3s CORE=penumbra2 VARIANT=probe`) with the
change in: **30.18 MHz, PASS at the 25 MHz target** on ECP5-85F sg6 — within
the typical 29–30 MHz band, above the 27 MHz floor, so no fmax regression from
the SR.S threading. This is also the establishing operating point for the full
gen2 machine probe (the bare-core probe was ~55 MHz; the cache/MMU/arbiter
layers are the limiter, not the compute cone).

## Hardware: WRSPR-SR dropped ISA-wide (RESOLVED)

WRSPR SR is reserved (illegal) on every generation: the kernel changes
`SR.S`/`SR.I` via exception entry / ERET / EI / DI and NZCV via flag-writing
ALU ops, never a direct SR write — and a direct `SR.S` write is the only SPR
write that would need WRSYS-style context-synchronization (`SR.S` gates
fetch-translation privilege). Dropping it removes that complexity entirely;
the value SPRs need no context-sync. RDSPR SR stays (NetBSD spl/status reads
it).

gen2 made it illegal first (piece 1a); gen1 (`cpu_core` traps `WRSPR SR` to
`VEC_ILLEGAL` at dispatch; `datapath` no longer asserts `spr_sr_load` for SPR
3) and the ISS (`WRSPR SR` raises `VEC_ILLEGAL`) now match, and the ISA docs
mark the encoding reserved. `isa/test_spr_sr` (its only purpose was `WRSPR
SR`) is removed; `isa/test_rdspr_sr` covers `RDSPR SR` + EI/DI and
`isa/test_wrspr_sr_illegal` checks the trap, both on every generation. The
`wrspr` capability tag now means exactly "value-SPR writes work" everywhere —
which is what lets piece 1c advertise `wrspr` on gen2 once the SCRn scratch
file lands.

The gen2 *rationale* docs (`penumbra2/{design-decisions,hazard-model,
control-decode}.md`) were reframed to match: `WRSPR SR` is no longer listed as
a live drain-commit / flag producer (ERET, EI/DI, WRSYS remain). The drain-
commit and S/I-serialization analysis that `WRSPR SR` used to motivate is
preserved as the *justification for reserving the encoding* — a direct `SR.S`
write would feed the MMU and IF1 IRQ logic out of pipeline exactly as ERET's
does, so it would need the same serialization and fetch re-sync, and reserving
it removes that case rather than building it.

## Hardware: build/test restructure to the BOARD×CORE matrix

`doc/internals/build-system.md` defines the target structure: the
four-axis build matrix, the `hw/rtl/machine/` integration layer, the
`isa/` conformance split for test programs, and the
`make fpga BOARD=<board> CORE=<generation>` porcelain. The
test-program side of the migration is done: programs live in `isa/` /
`penumbra1/` / `penumbra2/` with `; RUNNER:` / `; REQUIRES:` tags,
`hw/tools/run-prog-tests.py` drives `make test CORE=<core>` /
`make test-prog`, and `tb_penumbra2_prog` is the generic gen2 runner.
The FPGA side is done too: sources compose from `SRC_CORE_<gen>` /
`SRC_FABRIC` / `SRC_BOARD_<board>`, board tops live under
`hw/rtl/fpga/<board>/` (`ulx3s_top` became `ulx3s_penumbra1_top`),
the Makefile registry (`FPGA_TOPS`, per-top `FPGA_SRC_<top>`,
`FPGA_ROM_TOPS`/`FPGA_UCODE_TOPS` for hex embedding) hard-errors on
unknown BOARD/CORE/VARIANT combinations, and `TOP=` remains the
low-level escape hatch. The gen2 bare-core timing probe
(`ulx3s_penumbra2_probe_top`, `make fpga/timing BOARD=ulx3s
CORE=penumbra2 VARIANT=probe`) is in: core + `unified_mem`, IRQs on
buttons, commit/retire XOR-folded onto the LEDs so synthesis keeps
the design — the synthesize-after-every-change workflow for gen2,
in place *before* the BRAM L1 lands (Decision 11's 4-way-vs-leaner
choice is gated on the IF2 tag-compare/way-mux path at synthesis).
First bare-core build: ~55 MHz achieved on ECP5-85F sg6, critical
path in ID decode toward the fault-vector register.

The gen2 machine assembly is done: `machine_penumbra2` under
`hw/rtl/machine/` honors the program-end contract, the runners build
`machine_penumbra2_sim` (machine + `boot_rom` + the full SDRAM stack), the probe
variant wraps the machine — the IF2 tag-compare/way-mux path and the
L1↔L2 layer are in front of nextpnr; the first `make timing` run of
that configuration is pending — and the ISA-shaped gen2 programs
(smoke, branch, loadstore, fault, eret, syscall_trap) moved into
`isa/`. `test_intr` stays generation-pinned: its RUNNER tag selects
the IRQ-driving testbench, which is implementation-pinned by
definition. Remaining:

1. Opportunistic: extract `machine_penumbra1` from `machine_sim` /
   `ulx3s_penumbra1_top` so both wrappers share one integration
   (the sim-vs-FPGA congruence argument in the build-system doc).

The microarchitecture sub-variant axis is in: `CORE=penumbra<n>_<sub>`
(first instance `penumbra2_5`) builds its base generation's RTL with the
`CPU_VARIANT` core parameter set (entered as the `PENUMBRA_CPU_VARIANT`
define), across `make test` / `simulate-rtl` / `benchmark-rtl` / `fpga`.
`CORE_BASE`/`CORE_SUB` split the name; the variant inherits the base's
runner, capabilities, and program suite plus its own
`hw/sim/programs/penumbra2_5/`, and for fpga `TOP` names the per-variant
artifact while `TOP_MODULE` names the shared synth module (no duplicate
board top). The gen2.5 microarchitecture is built by **composition**:
`CORE=penumbra2_5` selects the gen2.5 fileset — gen2's leaf cells (ALU,
regfile, scoreboard, decoder, the MMU/TLB/cache stack, ...) shared
unchanged, plus the forked integration chain (machine shared; core /
spine / ID + EX assemblies forked into `hw/rtl/penumbra2_5/`) — so gen2
stays byte-frozen and never reads as a gen2.5 with its optimizations
switched off. The sim build resolves the forks via a variant `-I`
prepend, the fpga build via `SRC_CORE_penumbra2_5`; the cpuid name
("Penumbra/2.5") still rides the `PENUMBRA_CPU_VARIANT` define, and
cycle-exact tests gate on a `pinned-stalls` capability gen2 provides and
gen2.5 drops. Mechanism: `doc/internals/penumbra2/overview.md` (gen2.5
organization) and `doc/internals/build-system.md` (variant naming).
gen2.5 is no longer identity-only — see the feature status below.

### gen2.5 priority is ranked from the stall profile — real workload first

The build order below is **data-driven from the gen2 stall profile**,
re-measured on hardware after the perfctr stall-attribution fix (the
carried-cause counters — see the RESOLVED finding above). The pre-fix profile
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
counters (see the RESOLVED finding above) put those same loops at ~17-26%
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
  [the L2 cache design](internals/l2-cache.md#pipeline-hit_latency3-the-gen2-machine)
  and [the gen2 memory interface](internals/penumbra2/memory-interface.md#deferred-fill-speed-directions).
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

## Hardware: evaluate yosys-slang as the SystemVerilog frontend

Synthesis reads SystemVerilog through `sv2v`, which rewrites the whole
fileset to Verilog-2005 before Yosys sees it. The seam works, but every
line the design writes is translated first, and two of the workarounds
the flow carries exist only because of that translation — they describe
nothing about the hardware.

`hw/tools/inline_hex.py` is one. sv2v emits a zero-fill loop ahead of
each `$readmemh`, and Yosys lets the loop win, so every preloaded memory
— boot ROM, microcode, font, splash — would synthesize to zeros. The
script strips the loops back out of the generated Verilog.

The character generator's palette is the other. An unpacked array
indexed by a plain signal is the idiomatic way to write a lookup table;
sv2v flattens it to a packed vector whose part-select offset is the
index times the entry width, arithmetic the source never had. Yosys maps
that multiply onto a DSP block, so a 16-entry color table costs a
MULT18X18D. The workaround selects the entry by comparison instead.

`yosys-slang` reads SystemVerilog directly into RTLIL, removing the
translation step and, with it, the cause of both. Worth measuring rather
than assuming: the acceptance test is whether `inline_hex.py` can be
deleted and the palette can go back to a plain array index, with cell
counts on a full board top no worse than the current flow's.

One calibration so the evaluation stays honest — not every DSP surprise
is sv2v's. The cell address multiplies the character row by a constant
column count, and Yosys folds only power-of-two constant multiplies, so
80 reaches DSP inference whatever the frontend. Spelling that product as
the shifts its constant implies stays necessary either way, and a slang
trial that appears to remove it means something else changed.

The toolchain image is the prerequisite: the pinned OSS CAD Suite build
has no slang plugin (`read_slang` is not a command), so this needs the
container the wrapper scripts drive rebuilt or updated. The build-time
defines the flow passes at the sv2v step (`SDRAM_PHASE_DEG`,
`PENUMBRA_CPU_VARIANT`) need an equivalent on the new frontend.

## Compiler: graceful-fail on unsupported inline asm and vector IR

Today the GlobalISel IRTranslator crashes (`fatal error: unable to
translate instruction: call/ret`) when it encounters:

- Inline-asm constraint classes we don't implement -- `"g"` (any
  register/memory/immediate), `"m"` (memory operand), and tied
  `"0"` constraints where the tied operands have mismatched
  widths (e.g. i32 tied to i64).
- Vector-typed IR values (`__attribute__((vector_size(N)))`).
  Penumbra is a scalar target and rightly has no vector
  legalization, but the frontend still accepts vector types from
  GCC-extension source and hands them to the translator.

Impact is low on real code -- the crashes are reproducible only
from hand-written GCC-style sources that use these features
explicitly -- but the *right* behavior is a clean frontend
diagnostic, not a backend assertion.  Tracked tests are excluded
in `test/compiler/excludes.txt` under the corresponding sections.

Two fixes, independent:

1. **Inline-asm graceful fail.**  Teach
   `PenumbraTargetLowering::getConstraintType` to reject
   unsupported constraints with a diagnostic via
   `LLVMContext::diagnose` / `report_fatal_error` with the user
   source location, rather than letting IRTranslator assert.
   Optional follow-up: implement `"g"` as "treat as `r`" so the
   optimization-barrier idiom works everywhere.

2. **Vector graceful fail.**  Add a frontend-level check (or
   LegalizerInfo with a clear "unsupported" action + diagnostic)
   so `vector_size` attributes produce a compile error naming the
   source file, not a backend crash.

## Compiler: s128 legalization for wide packed bitfields

`#pragma pack(1)` structs whose total bitfield width exceeds 64
bits (e.g. `long long a:43; b:22;` or `int a:18; b:1; c:24; d:15;
e:14;`) get lowered by clang to integer-wide load/store on a
non-power-of-two scalar.  LLVM's legalizer widens to the next
power of 2 — s128 — which our backend does not handle.

Peer 32-bit GISel ports are in the same state: ARM's GISel
legalizer has no s128 rules at all, and RV32's is gated behind
`ST.is64Bit()` with an explicit `FIXME` for libcall return
handling.  The feature isn't implemented anywhere in the 32-bit
GISel ecosystem.

Plausible implementation: add
`.narrowScalarIf(typeIs(0, s128), changeTo(0, s64))` to the
LOAD/STORE/ZEXTLOAD/SEXTLOAD/SHIFT/ADD-family/TRUNC rule groups,
so s128 chains through our existing s64→s32 narrowing.  Needs
care around G_ZEXTLOAD when the memory size equals the narrowed
scalar size — LegalizerHelper may try to split the memory access
too, which is not what we want here.

Tracked tests: `testcase-InstCombine-1.c`, `pr57344-3.c`,
`pr57344-4.c` (excluded in `test/compiler/excludes.txt`).

## Compiler: compare-elim splice past ADC/SBC — RESOLVED; frame-index hazard open

The compare-elimination peephole's backward walk skipped SR readers,
so its splice could move a carry-producing `ADD`/`SUB` past the
`ADC`/`SBC` consuming its carry, breaking i64 arithmetic.  Fixed:
the walk bails on any gap SR reader; the repro is a negative case in
`test/CodeGen/Penumbra/compare-elim.ll`.  Binaries built between the
peephole extension and the fix are suspect until rebuilt — that
window covers every installed userland, so the fix reaches the
board only with a distribution rebuild.

Still open from the same audit
([`doc/llvm-flag-reuse-notes.md`](llvm-flag-reuse-notes.md)): the
`eliminateFrameIndex` large-offset path emits a post-RA `ADD` that
clobbers SR inside a live-flags window for frames over 32 KiB.
Flags cannot be saved in software (`WRSPR SR` traps), so the fix is
relocation — check SR liveness at the insertion point and emit the
materialisation above the nearest earlier SR def.

## Compiler: compare-elim does not look through COPY

`optimizeCompareInstr`'s backward walk takes the first instruction that
defines the compared register as the flag producer, and gives up unless
it is flag-setting.  A `COPY` in between stops it —
`definesFlagsFromResult(COPY)` is false — even though a `COPY` neither
touches SR nor changes the value.

The shape reaching it today comes from `selectZExt`, which emits a
`COPY` rather than a mask when zero-extending a carry-out that
`emitCarryToValue` already made 0/1:

```
%10:gpr = ANDi %27, 1, implicit-def $sr   ; sets Z from the result
%19:gpr_allocatable = COPY %10
TESTi %19, 1, implicit-def $sr             ; recomputes the same Z
```

Three sites in a 120-file Doom build, more in i64-heavy code.  The
`SUB`-then-`COPY`-then-`CMP` form named in the peephole's own header
comment is the same gap.

Following the `COPY` means retargeting the walk to its source, and that
collides with the splice: the `COPY` then *reads* the producer's def,
so splicing the producer down past it would leave the `COPY` reading an
undefined register.  The way out is that the splice is keyed on
adjacency when what it exists for is making the producer the most
recent SR write.  Keyed instead on "some instruction in the gap
modifies SR", the two compose — a `COPY`-only gap needs no splice and
its reads are harmless, while a gap that does clobber SR still splices
and the existing "gap must not read the compared register" check still
rejects the unsafe case.

Treat as delicate: this is the walk that produced the ADC/SBC splice
miscompile above.  The negative cases belong in
`test/CodeGen/Penumbra/compare-elim-mask.mir` beside the mask ones.

The same re-keying covers a gap that is not a `COPY`:
`sub r, x; stw r, [...]; cmp r, 0`, where the store reads the compared
register without touching SR.  106 kernel sites, including
`KPREEMPT_ENABLE` on every `userret`.

Separate and not covered by the walk: 164 kernel sites repeat an
identical compare at the head of a fall-through block whose
predecessor already set the same flags.  That redundancy crosses a
block boundary, where a backward walk cannot see it.  SQLite's
repeats (161 same-block, 407 across labels) come mostly from the i64
compare expansion in "64-bit relational compares expand to select
diamonds".

## Compiler: GISel poison-flag hygiene is an LLVM rebase gate

The known upstream GlobalISel wrap-flag miscompile families are
audited in
[`doc/llvm-gisel-upstream-miscompile-exposure.md`](llvm-gisel-upstream-miscompile-exposure.md):
both flag-corruption mechanisms actively deposit false `nuw` flags
in our pipeline today and are harmless only because this LLVM's
known-bits code never reads them.  A rebase (or local flag-aware
known-bits work) converts them to live miscompiles with no fallback
to hide behind.  Before any LLVM upgrade, confirm the combiner
poison-flag drop is in the tree or carry it; backport the lowerBswap
mask fix opportunistically; re-run the audit's probes after any
rebase.

## Compiler: s1 booleans lose their comparison through logic ops — RESOLVED

Booleans are register-width now: `G_ICMP`/`G_FCMP` produce s32 and
`G_BRCOND`/`G_SELECT` consume it, matching AArch64 and RISC-V, with
`setBooleanContents(ZeroOrOneBooleanContent)` declaring the 0/1
guarantee.  The carry linking `G_UADDO` to `G_UADDE` stays s1: it is
the SR.C bit rather than a value, and `narrowScalarAddSub` creates
those carries as s1 unconditionally, so any other width would only be
widened straight back apart.  −1.17% on a 120-file Doom build.

The original diagnosis here was wrong and is worth recording, because
it sent the first attempt at the wrong target.  The ext/trunc pair
around a logic op costs nothing — `G_ANYEXT`/`G_TRUNC` select to
`COPY` and coalesce away, and `(a>=b)|(c>=d)` always selected to
`cmp`/`adc` pairs plus an `or`.  Two other things were doing the
damage:

- **Signed compare-to-value.**  InstCombine rewrites `x >> 31` into
  `icmp slt x, 0` whenever it feeds an `i1` operator, and
  `selectICmpToValue` bails on every signed predicate.  Independent of
  boolean width; still open, see the entry below.
- **`BitWidth > 1` in `GISelValueTracking`.**  The known-bits rule for
  a comparison result is gated on the result being wider than one bit,
  so `ZeroOrOneBooleanContent` was inert while `G_ICMP` produced s1.
  That is why the earlier `setBooleanContents` experiment measured no
  benefit — the two changes only work together.

`lowerThreewayCompare` picks the arithmetic form over the select chain
once boolean contents is declared, which regressed `scmp_i32`; the
`preferSelectsOverBooleanArithmetic` hook holds it on the select
chain.

Widening leaves `G_AND %bool, 1` behind, which `redundant_and` cannot
drop because a `G_FREEZE` blocks known-bits — an upstream hole written
up in
[`doc/llvm-gisel-freeze-known-bits.md`](llvm-gisel-freeze-known-bits.md).
Rather than wait for it, the compare-elimination peephole erases the
`TESTi` those masks feed, since the mask already set the same Z.

## Compiler: signed comparisons have no branchless materialisation

`selectICmpToValue` resolves unsigned and eq/ne predicates to one
compare plus a carry read.  Signed predicates have no flag that answers
them, so they fall through to the `SELECT_CC` diamond — `lli 1` /
`lli 0` / `cmp` / `Bcc` / `mov`, six instructions and a data-dependent
branch.  `emitSelectCC` shares the diamond, so `G_SELECT` on a signed
predicate pays the same.

The sign-bit tests are done: `slt x, 0` is `SHR #31` and `sge x, 0` is
`NOT` then `SHR #31`, as two patterns in `PenumbraGISel.td`.  They
matter beyond their site count because InstCombine rewrites `x >> 31`
into `icmp slt x, 0` whenever the value feeds an `i1` operator, so
source written as a shift still arrives as a comparison.

What is left has no cheap closed form:

- `sgt x, 0` and `sle x, 0` compare against zero but test more than the
  sign bit.  `((0 - x) & ~x) >> 31` is five instructions — branchless,
  but no smaller than the diamond.  Worth it only where the branch is
  hot enough to pay for itself.
- Two variable operands have nothing.  Biasing to unsigned
  (`(a ^ 0x80000000) <u (b ^ 0x80000000)`) costs more than the diamond
  once the constant is materialised.

Remaining sites, counted on binaries built before the sign-bit
patterns landed:

| binary | diamonds | of which sign-bit tests |
|--------|---------:|------------------------:|
| NetBSD `GENERIC.DEBUG` | 565 | 139 |
| `libc.a`               | 408 |  83 |
| Doom, 120 files        | 117 |  11 |

The kernel's are scattered across calendar arithmetic and filesystem
block mapping (`clock_secs_to_ymdhms`, `ufs_bmaparray`, `ffs_balloc`),
not in syscall entry, trap handling or `copyin`/`copyout` — so expect
code size rather than a benchmark to move.

## Compiler: compare and branch selection misses immediate and TEST forms

`emitCompare` folds an immediate only when the constant fits uimm16 as
given, and `selectBrCond` either consumes a compare or tests a
materialized boolean.  Four shapes pay for that (static counts,
2026-09-13):

| Source | Emitted | Wanted | Sites |
|---|---|---|---|
| `x >= 0` (`icmp sgt x, -1`) — DONE | `llis t,-1; cmp x,t; ble` | `cmp x,0; blt` | kernel 274, SQLite 612, Doom 86 |
| `(x & C) != 0` — DONE | `mov t,x; and t,C; beq` | `test x,C; beq` | Doom 140 |
| branch on a non-compare `i1` whose true edge falls through — DONE | `lli t,1; xor t,b; test t,1; bne` | `test b,1; beq` | kernel 848 |
| `x == -1` | `llis t,-1; cmp x,t; beq` | `not t,x; beq` | Doom 53 |

- **Immediate adjustment — DONE.**  `sgt x, -1` is `sge x, 0`.
  `canonicalizeCompare` moves the predicate and the constant by one
  when only the adjacent constant fits the immediate (`sgt C` ↔
  `sge C+1`, `slt C` ↔ `sle C−1`, and the unsigned pairs), for
  branches and selects alike, and refuses a step that would wrap in
  the predicate's signedness.  Shape coverage: `compare-imm-test.ll`,
  `GlobalISel/select-compare-immediate.mir`.  The `llis -1; cmp` pairs
  left (kernel 356, `libsqlite3` 25, Doom 51) are `x == -1` and the
  byte and halfword sign tests in "signed sub-word loads in relational
  compares miss LDBS/LDHS".
- **Branch on N for sign tests.**  After `cmp x, 0` V is clear, so
  `bmi`/`bpl` are exact for `x < 0`/`x >= 0`.  Reading N alone also
  lets compare elimination drop the compare after a producer whose V
  means something else — the `XOR` in Doom's `R_PointOnSide`, the
  `SUB` in `R_AddLine` and `R_CheckBBox`.
- **TEST fold — DONE.**  The `mov` existed because `AND` is
  destructive and `x` stays live.  A branch on `(x & m) == 0` or
  `!= 0` whose AND has no other user now selects `TESTi x, m`, or
  `TEST x, m` for a wider mask.  Selects keep the AND.  Copy-and-mask
  tests went from 1,243 to 164 in the kernel, 706 to 35 in
  `libsqlite3` and 130 to 15 in Doom.

  Together with the immediate adjustment, measured statically on the
  hot-path review corpus (2026-09-13): kernel −1,877 instructions
  (`.text` −7,508 B, 18 fewer callee-saved saves), `libsqlite3`
  −1,461, Doom −253.  `trap` is 12 instructions shorter.  31 kernel
  functions grew, the largest `do_sys_mount` by 20 of 556, all in
  spills and reloads.
- **Inverted boolean — DONE.**  The branch-inversion combine wraps
  the condition in `G_XOR %v, 1` when the true edge is the layout
  successor.  A compare absorbed it into the predicate, but any other
  `i1` — a call result, a `bool` load, a phi — reached `selectBrCond`
  with the XOR still in place, and selection materialized and tested
  it.  `selectBrCond` now looks through single-use `XOR c, 1` and
  `AND c, 1` wrappers and flips the branch sense.  The corpus's
  `lli 1; xor` pairs dropped from 1,071 to 32 in the kernel, 199 to
  42 in `libsqlite3` and 88 to 18 in Doom; those left feed values,
  not branches.  Shape coverage: `brcond-inverted-bool.ll`.  RISC-V's
  GlobalISel output has the same `xori`, so this is upstream-worthy.

## Compiler: share the hi-word compare in i64 three-way compares

The `G_SCMP`/`G_UCMP` lowering (`.lower()` →
`LegalizerHelper::lowerThreewayCompare()`) expands the i64 case
verbosely: 16 BBs — two s64 ICMPs each become a three-block hi/lo/eq
diamond, materialized as four `mov` selects.  A peephole that shares
the hi-word compare across the two ICMPs would shrink it.  Not on any
hot path (qsort's comparator is the i32 case); low priority.  The
two-way relational compares have the same problem and are hot — see
the next entry.

## Compiler: 64-bit relational compares expand to select diamonds

`LegalizerHelper::narrowScalar` splits an s64 `G_ICMP` into
`select(hi == hi', ult(lo, lo'), pred(hi, hi'))`.  Selection builds
the signed half with a branch diamond (`selectICmpToValue` has no
signed path), the `G_SELECT` with a second one, and emits the
high-word compare twice.  A branch on signed `a < b` takes 12
instructions on its path:

```
cmp r1,r3; mov r1,zero; adc r1,zero; lli r3,1; lli r11,0
cmp r2,r4; blt ...; cmp r2,r4; bne ...; test r1,1; bne ...
```

A borrow chain does it in four: `cmp alo,blo; mov t,ahi; sbc t,bhi;
blt` (`blo` unsigned).  `MOV` leaves the flags alone, so the copy can
go anywhere before the `SBC`.  Only `lt`/`ge` come out of the chain
directly — Z reflects just the high word — so `gt`/`le` swap operands.

| Source | Now | riscv32 SelectionDAG | Borrow chain |
|---|---:|---:|---:|
| branch on signed `<` | 11–13 | 3–5 | 3 |
| branch on unsigned `<` | 10 | 3–5 | 3 |
| value of signed `<` | 16 | — | 5 |
| value of unsigned `<` | 12 | — | 5 |
| branch on `v < 0` | 11 | 1 | 2 |
| `(v & 0xffffffff) != v` | 16 | 3 | 3 |

The borrow-chain branch counts assume a flag-only `SBC`; add one for
the `MOV`.  Constants need no chain at all: `v < 0` is
`cmp hi,0; blt`, and a right-hand side with a zero low half reduces
to a high-word compare.

Reach (static, 2026-09-13): `libsqlite3` has 470 on variable
operands and about 130 against constants.  That includes two per
`sqlite3BtreeMovetoUnpacked` bisection step (rowid against cell key),
`vdbeRecordCompareInt`, and 42 in `sqlite3VdbeExec`.  The chain saves
about 20 cycles per compare on penumbra1 and 9 on penumbra2_5.  The
kernel's `off_t`, `time_t` and 64-bit counter code has the same shape;
not counted.

SelectionDAG's `IntegerExpandSetCCOperands` (`LegalizeIntegerTypes.cpp`)
uses `SETCCCARRY` where the target provides it — ARM does — and
special-cases constant halves.  GlobalISel has no counterpart, so this
wants a `doc/llvm-*.md` write-up.

Fix direction: a custom s64 `G_ICMP` legalization or pre-legalizer
combine into a low-half `G_USUBO` feeding a high-half borrow
subtraction, selected as `CMP`/`SBC` with the branch or value read
from its flags, and constant right-hand sides folded to a high-word
compare.

**ISA documentation decision first.**  The encoding defines the F
(flag-only) bit for every Format R operation, so a flag-only `SBC` is
expressible, but `instruction-set.md` names only `CMP` and `TEST`.
Using it needs a named mnemonic in the doc and an `isa/` conformance
test run on every core; without it the chain keeps the `MOV`.

## Compiler: PIC global addresses are recomputed inside loops

Under PIC, a loop that indexes a global array recomputes the array's
address on every iteration: the `LLI`/`LUI`/`ADD Rd, PC` triple sits in
the loop body even when the source hoists the pointer into a local.
Found in skrolli2026's sine lookup (`-fPIE -O2`, penumbra2_5): a
320-pixel row of table lookups is 15 instructions per pixel where 12
do the work, so a fifth of the loop is address arithmetic.  Any
per-pixel loop over a table pays the same three instructions.

Two passes combine to cause it, found with `-print-after-all`:

1. The GlobalISel Localizer clones the `G_GLOBAL_VALUE` into its using
   block, which is the loop body; the preheader copy is then dead.
2. Nothing hoists it back.  The direct PC-relative path selects the
   `PICLLI`/`PICLUI`/`PICADDPC` triple ("PC-Anchored Relocation Pairs"
   in `doc/system/abi.md`), and none of the three has a pattern or an
   explicit `hasSideEffects`, so TableGen infers
   `UnmodeledSideEffects` for all of them and MachineLICM leaves them
   where they are.  The static-mode `PseudoMOVADDR` does not have the
   problem: it is `Rematerializable` with no side effects.

Moving the triple is safe since the anchors became label-relative;
only duplicating `PICADDPC` is not (it would redefine its label).  So
the first thing to try is an explicit `hasSideEffects = 0` on the
three pseudos, and checking that MachineLICM then hoists them.  The
fuller fix is a single rematerialisable pseudo for the direct path,
expanded after register allocation into the anchored triple, the way
`PseudoMOVADDR` works for static code.  Keeping the Localizer from
sinking a global into a deeper loop would also remove the symptom, but
MachineLICM should be able to undo it either way.

Test shape: a loop storing `table[f(i)]` from a hidden global table,
checking that no `ADD ..., PC` remains in the loop block.  An empty
`asm("" : "+r"(p))` on the pointer before the loop reaches the
12-instruction body, which is the measure for the fix.

## Compiler: aggregate-ABI rework — small structs in registers — RESOLVED

`doc/system/abi.md` ("Argument Passing" / "Return Values") specifies
the slot-based aggregate convention: aggregates ≤ 4 bytes travel by
value in one slot, 5–8 bytes in two slots (register/stack straddle
allowed), > 8 bytes by reference to a caller-owned copy; returns
≤ 8 bytes come back in R1/R1:R2, larger ones via hidden sret pointer
with R1–R4 undefined at return.

Implemented in clang as `PenumbraABIInfo`
(clang/lib/CodeGen/Targets/Penumbra.cpp, commit `8fb7d7dc`), replacing
the earlier `DefaultABIInfo` that made every aggregate indirect and
every aggregate return sret.  The classes follow the RISC-V ILP32
pattern: ≤ 4 bytes → Direct(i32), 5–8 → Direct([2 x i32]), > 8 →
indirect **non-byval**, with the same classes for returns
(`RetCC_Penumbra` assigns i32 to [R1, R2]).  Making the indirect class
non-byval is the keystone: clang materializes the by-value copy in IR
and the backend only ever sees a plain pointer, so the two byval-path
defects below cannot arise from clang-emitted code (`normalizeVarArgByVal`
and the framework byval handling are now dead for it).

The three defects the `test/compiler/penumbra-abi/` tests pinned down:

- **Missing byval copy for register-slot aggregates** (C pass-by-value
  silently became pass-by-reference) — gone with the non-byval
  classification; `abi-aggregate-{mutate,const,value}.c` pass.
- **Stack-positioned byval corrupts trailing arguments** — same root
  cause, gone; `abi-aggregate-boundary.c` (the stack straddle) passes.
- **Outgoing stack stores expand byte-by-byte** — independent backend
  bug: `PenumbraOutgoingValueHandler` tagged the slot with a bare
  `MachinePointerInfo::getStack()`, so `inferAlignFromPtrInfo` returned
  Align(1) and every stack argument (scalars included) lowered to the
  byte-store expansion.  Fixed in `bbd64bffed05` by deriving the
  alignment from the Align(4) SP and the 4-aligned slot offset
  (`commonAlignment`).

Coverage: execution tests `test/compiler/penumbra-abi/*.c`; shape
tests `clang/test/CodeGen/Penumbra/aggregate-abi.c` (frontend
coercion) and `test/CodeGen/Penumbra/{aggregate-args,outgoing-stack-arg-align}.ll`
(backend register placement + stack-store width).  The `struct-ret-1.c`
exclusion was re-triaged and removed — it passes at every opt level.

## Compiler: two scalar miscompiles surfaced by rebuilding compiler-rt — RESOLVED

Both symptoms (`__divdf3` quotients 1 ULP short at -O2/-O3;
`__umodXi3` wrong remainders at -O0) plus the previously separate
`pr23135.c` -O0 failure turned out to be **one bug**: the
instruction selector's carry-in fusion deleted flag-producing
ADD/SUBs whose 32-bit sum was dead but whose carry/borrow was live.
`carryInAlreadyLive` let a `G_UADDE`/`G_USUBE` select to a bare
ADC/SBC reading SR.C directly, dropping the use of the s1 carry
vreg; bottom-up selection then reached the producer `G_UADDO`,
found no remaining def uses, and erased it as trivially dead —
along with its input cone.  In compiler-rt this hit `wideMultiply`
whenever the low product half is unused (`fp_div_impl.inc`'s
`dummy`): the `plolo` multiply and the `r1` carry adds vanished,
making pre-round quotients 2 ULPs short where the error analysis
allows at most ~1.5 (the rounding step itself — the original
suspect — was correct all along, and clawed one ULP back).

Fix: `carryInAlreadyLive` rejects fusion when the producer's sum
register has no uses (the fallback materializes the carry through a
GPR, keeping the producer alive).  Regression test
`test/CodeGen/Penumbra/uaddo-dead-sum.ll` pins all three shapes
(single carry, chained carries, borrow); full investigation notes in
`doc/llvm-divdf3-rounding-bug.md`.

Found along the way, still open: the bare 64×64→128 mulhi idiom
(`wideMultiply` reimplemented as a function returning only the high
half) is recognized by the IR optimizer into an i128 multiply that
crashes the legalizer — see "Compiler: G_ZEXT s128 from the mulhi
idiom fails to legalize" below.

## Compiler: G_ZEXT s128 from the mulhi idiom fails to legalize — RESOLVED

A function that computes the high 64 bits of a 64×64 product from
32-bit partial products and returns only that half (the classic
mulhi shape) gets idiom-recognized by AggressiveInstCombine's
ungated `foldMulHigh` into `zext i64 → i128; mul i128; lshr 64;
trunc`, which ICEd the legalizer.  C23 `_BitInt(128)` reached the
same gap directly.  (`compiler-rt`'s `wideMultiply` escapes
recognition because it returns both halves through out-params; the
32→64 flavor of the same transform is a win for us — a hand-written
`mulhi32` becomes a single MULU.)

Fixed in three parts, no actual i128 support needed (GISel narrows
iteratively s128 → s64 → s32 through the same paths s64 uses):
extension narrowing widened from `typeIs(s64)` to
`scalarWiderThan(32)`; extending loads with memory wider than
register width (`G_ZEXTLOAD s128 ← s64`, formed when a load feeds
the zext) lowered to plain load + extension; and the missing
aligned-pow-2 case added to the generic
`LegalizerHelper::lowerLoad` (it previously assumed lower() on an
extload always meant an unaligned split).  Test:
`test/CodeGen/Penumbra/i128-mulhi.ll`.

**Upstream-worthy:** the `lowerLoad` gap is generic — riscv32
GlobalISel ICEs on the identical `G_ZEXTLOAD s128 ← s64` from the
same IR (verified against an in-tree RISCV llc build), via the
sibling `// FIXME: Need to split the load.` hole in
`narrowScalar`'s extload case.  Full upstream-facing analysis,
reproducers, target survey, and proposed two-part patch:
`doc/llvm-gisel-wide-extload-legalization.md`.

## Compiler: PIC GOT anchor was position-dependent under code motion — RESOLVED

Dynamic NetBSD died at boot: init took a SIGBUS storm
(`Process (pid 1) got sig 10`, no progress) the instant it started.
Bisected to userland (old kernel + new userland reproduced; new
kernel + old userland booted), then to libc's `dl_iterate_phdr`
parsing the auxv — its switch cases stored auxv values through the
wrong GOT slots.

Root cause was the PIC GOT materialization
`LLI %got_pcrel_lo16(sym-8); LUI %got_pcrel_hi16(sym-4); ADD Rd, PC`,
where the `ADD`'s own PC is the relocation anchor and the **-8/-4
addends hard-coded the assumption that the ADD sits 8 bytes after the
LLI**.  Nothing enforced that adjacency.  The new GlobalISel
Localizer pass (`4c016efc4222`) clones `G_GLOBAL_VALUE` into each
using block, so switch cases storing through different globals ended
in identical `ADD/LDW/STW` tails — which BranchFolder tail-merged,
fusing the anchor `ADD`s of separate cases and leaving two of three
`LLI/LUI` pairs measuring against a PC 8 bytes off.  Each computed a
neighboring GOT slot; the dereferenced "address" was an unrelated
rodata pointer, and the store faulted.  Pre-Localizer, GISel CSE
materialized each global once, so duplicate triples never existed for
tail-merging to find — the latent bug had no trigger.

Fixed by making the sequence position-independent the way ARM32
(`PICADD` + `.LPC` labels) and RISC-V (`%pcrel_lo(.Lpcrel_hi)`) do:
the addend now names the anchor via label arithmetic
(`%got_pcrel_lo16(sym - .LPC0_0)` with `.LPC0_0:` on the ADD) instead
of a fixed constant.  Selection emits paired pseudos
(`PICLLI`/`PICLUI`/`PICADDi` carriers + `PICADDPC`/`PICMOVPC` anchors,
`isNotDuplicable`) sharing a per-function pclabel id; the AsmPrinter
places the label and lowers the label-difference operand, which the
assembler folds into the relocation addend (`A = P - Q`).  The linker
formula `S + A - P` is unchanged — no lld or relocation-table change.
Covers the GOT-global, TLS-GD, block-address, and jump-table PIC
paths.  Spec: `doc/system/abi.md` "PC-Anchored Relocation Pairs".
Tests: `test/CodeGen/Penumbra/pic-anchor-tail-merge.ll` (the
switch/tail-merge shape) and `test/MC/Penumbra/got-pcrel-label-anchor.s`
(addend folding across adjacent, branched, and same-section-local
layouts).  Validated by booting dynamic NetBSD to the single-user
shell (init/`/bin/sh`/`sysctl` all run, zero faults).

**Test-coverage gap — addressed:** `make test-compiler` runs
`penumbra-unknown-none` bare-metal, non-PIC, so it never exercised GOT
materialization — which is why a PIC-only miscompile reached a libc
this fundamental before anything caught it.  `make test-compiler-pic`
now runs a curated `-fPIC` set (`test/compiler/penumbra-pic/`) through
the same harness, linked static at a fixed address so lld resolves the
GOT at link time (no runtime relocator needed for the flat ISS image)
while the GOT-indirect codegen is still exercised.  The tests are
self-checking and were confirmed to fail when the anchor pairing is
deliberately broken (each catches a value landing in the wrong
GOT-loaded global / jump-table target).  Still open: a true dynamic
PIE leg (with `__tls_get_addr` and runtime relocation) would also cover
TLS-GD, which the static-link approach cannot reach.

## Userland: ssh dies with SIGBUS — RESOLVED (lld PLT reach)

Root-caused from the first core dump the port ever wrote: PC held
instruction-encoding garbage loaded through a libcrypto PLT stub.
The stub's PC-plus-two-16-bit-immediates addressing reached at most
~98 KB and lld's split helper silently truncated beyond that;
libcrypto's .plt-to-.got.plt span is ~232 KB, so its every stub
loaded neighboring PLT code bytes as a function pointer.  Fixed in
lld (full 32-bit LLI/LUI displacement; regression
`lld/test/ELF/penumbra-plt-far.s`).  A linker change is invisible to
nbmake's dependency tracking: the fix reaches installed binaries
only through a clean userland rebuild, and any library that large
built before the fix is suspect until relinked.


## Kernel: guard page for kernel stack overflow

The kernel u-area (`UPAGES = 4`, 16 KB) has no guard page, so stack
overflows corrupt whatever lives immediately below the u-area before
eventually manifesting as a confusing nested TLB miss inside
`_trap_common` (and a double-fault BREAK).  We already sized USPACE
above the worst -O0+DIAGNOSTIC frames, but a silent corruption
window still exists.

NetBSD supports per-arch opt-in guard pages via the
`__HAVE_CPU_UAREA_ROUTINES` hook.  See
`netbsd/sys/arch/x86/x86/vm_machdep.c:cpu_uarea_alloc()`: it
allocates `USPACE + PAGE_SIZE` from `kernel_map`, then
`pmap_kremove()`s the redzone page and `uvm_pagefree()`s the backing
PA so any touch bus-faults immediately.  `cpu_uarea_free()` is the
inverse.  amd64 uses UPAGES=5 (4 real + 1 redzone); i386 with
redzone enabled uses 3 (2 + 1); KASAN/KMSAN builds use more.

Implementation sketch for Penumbra:
1. Define `__HAVE_CPU_UAREA_ROUTINES` in `include/cpu.h`.
2. Add `cpu_uarea_alloc(bool system)` / `cpu_uarea_free()` to
   `penumbra/machdep.c`, mirroring the x86 pattern.  We need a
   leading guard (fault on underflow from the top of stack growing
   down) — allocate `USPACE + PAGE_SIZE`, strip the *first* page.
3. Optionally add a trailing guard too — allocate `USPACE + 2*PAGE_SIZE`
   and strip both.  amd64 does both.
4. Adjust `penumbra_lwp0_init()` in `startup.c` to use the same
   layout for lwp0's uarea so the boot stack is guarded from the
   start, not only from first `fork()`.

After this lands, kernel stack overflow produces a clean bus fault
with EPC pointing at the offending instruction, not a nested TLB
miss in the trap handler.

## Kernel: fork() is unreasonably slow

`pbench kernel fork_exit` reports **~670 ms per fork+exit+wait round
trip** on the ULX3S FPGA at 25 MHz (see
`benchmark/netbsd-bench/BASELINE.md`).  That's two orders of magnitude
beyond what the workload should cost, and it makes the system painful
to use: `/etc/rc` runs many short commands sequentially, single-user
boot stalls visibly, and any shell pipeline is sluggish.  For
comparison, `pipe_pingpong` (which exercises 2 context switches + 4
syscalls per round trip) reports ~19 ms — so context-switch cost
alone is ~30× cheaper than a fork.  The bulk of the 670 ms is
fork-specific work, not generic scheduler/syscall overhead.

### Where the time goes — full attribution

Nine MD-side phases are instrumented via the `machdep.fork_timing`
sysctl (`struct fork_timing_snapshot` in `<machine/fork_timing.h>`).
Two waves of measurement, both on the ULX3S FPGA @ 25 MHz, median
over 8 trials × 10 forks per trial:

| Phase                  | per-fork    | calls/fork | % of total |
|------------------------|-------------|-----------:|-----------:|
| `cpu_lwp_fork`         | 0.11 ms     | 1          | 0.02 %     |
| `pmap_create`          | 5.74 ms     | 1          | 0.86 %     |
| `pmap_remove_all`      | 117.4 ms    | 1          | 17.5 %     |
| `pmap_destroy`         | 4.14 ms     | 1          | 0.62 %     |
| `pmap_enter` (agg)     | 200.3 ms    | 181        | 29.9 %     |
| `pmap_protect` (agg)   | 0.86 ms     | 4          | 0.13 %     |
| `pmap_copy_page` (agg) | 28.0 ms     | 10         | 4.19 %     |
| `pmap_kenter_pa` (agg) | 0.07 ms     | 1          | 0.01 %     |
| `uvm_fault` (agg)      | 586.8 ms    | 51         | 87.6 %     |

`uvm_fault` is the *inclusive* outer bracket (the call site in
`trap.c`).  `pmap_enter` and `pmap_copy_page` are nested inside it.
Subtracting the nested leaves:

  **`uvm_fault` exclusive (MI internals) = 587 − 200 − 28 = 359 ms.**

That 359 ms is the page-fault machinery `uvm_anon_alloc`,
`uvm_pagealloc`, `amap_add`, trap-entry/exit overhead — all MI code,
unreachable from MD without forking the upstream NetBSD tree.

### What lives where

**MD-attributable share — ~54 % of total fork+wait time:**

- `pmap_enter` × 181 calls × ~28 k cyc each (29.9 %)
- `pmap_remove_all` 117 ms (17.5 %)
- `pmap_copy_page` 28 ms (4.2 %)
- `pmap_create` + `pmap_destroy` + others (~1.6 %)

**MI-bound share — ~46 % of total:**

- `uvm_fault` exclusive (359 ms): page-fault dispatch internals
- Scheduler enqueue, struct proc/lwp setup, filedesc dup, signal
  machinery, wait reaping — not separately instrumented, but
  bounded above by `total − sum(MD)` ≈ 670 − 360 = 310 ms.

### What I got wrong on the first wave

- **`pmap_protect` was hypothesised to dominate** as the "COW
  marking pass."  Measured at 0.86 ms / 4 calls / fork — NetBSD
  uses *lazy* COW, the protect-on-fork pass doesn't exist.  Dead
  end; don't optimise `pmap_protect`.
- **`pmap_kenter_pa` was hypothesised to soak uarea-allocation
  cost.**  Measured at 66 µs / 1 call / fork.  uarea pool caching
  is already doing its job; one kenter per fork is the L1 KVA
  wire from `pmap_create`, not the uarea pages (those are
  inherited from the pool warm).

### Why `pmap_enter` is so expensive per call

181 calls × ~155 µs each = 28 ms × 51 faults * 3.5 entries/fault
≈ the observed 200 ms.  Per-call 28 k cycles = 1.1 ms is a lot for
a single PTE install.  Plausible contributors, in order of suspicion:

1. **`pool_get` for the PV entry.**  Mutex acquisition + freelist
   walk + occasional pool growth path.  Could be hundreds of cycles
   even on the warm path.
2. **`pmap_pte_lookup` scratch-window reload.**  Every call does a
   WRSYS to remap pinned TLB slot 3 to the L2 page, even when the
   previous lookup was for the same L2.  At 181 calls/fork with
   most adjacent VAs sharing an L2, an L2-cache here could fold
   the WRSYS away on most calls.
3. **`pmap_tlb_invalidate`** — one WRSYS per call.
4. **`icache_invalidate`** — full-cache invalidate if the mapping is
   executable.  Many of the child's mapped pages are executable
   (libc text), so this could fire on most calls.

A second-stage instrumentation inside `pmap_enter` (bracket the four
sub-phases above) is the cleanest way to attribute the 1.1 ms.

### Why `pmap_remove_all` is suspiciously large on its own

~118 ms ≈ 2.95 M cycles for an init-sized address space (~2000 valid
pages, sparse over ~10 valid L1 slots).  That works out to ~1500
cycles per visited *valid* PTE — far more than the algorithm
describes (load+branch on invalid entries; PV-list walk + stat
decrement on valid ones).  Likely culprits: the per-L2 cached SDRAM
walk over 1024 entries each (256 cache lines × first-touch SDRAM
latency), and `pv_remove`'s SLIST traversal hitting cold lines.
Scratch-window cache (item 2 in "Quickest wins" below) would help
directly since adjacent L2 walks share PA.  Worth a closer look in
its own right after the bigger gap is closed.

### Quickest wins, ranked by leverage

Now that attribution is complete, candidates ordered by leverage on
MD-attributable time:

1. **Attribute `pmap_enter`'s 1.1 ms/call.**  Bracket the four
   sub-phases (PV pool_get, scratch-window reload, TLB invalidate,
   icache invalidate) inside `pmap_enter` and re-run the bench.
   Without that breakdown we don't know which fix lands the biggest
   share of the 200 ms (~30 % of total).  Cheap follow-up; should be
   the next move.
2. **Scratch-window caching across `pmap_l2_map`.**  A one-entry
   cache that skips `pmap_scratch_map` when the target L2's PA
   matches the previous lookup.  Applies to `pmap_enter`,
   `pmap_remove_all`, and `pmap_extract`/`pmap_clear_modify`.
   Likely worth tens of ms.
3. **`pmap_remove_all` early-out per L2.**  Stop walking once
   `pm_stats_resident` reaches zero; most empty L2 slots beyond
   that point don't need scanning.  Probably a few-ms win.
4. **Pre-populating the child's pmap with parent's R-O pages.**
   Would reduce the 51 faults / fork to a handful.  Bigger refactor
   (shared L2 or eager L1-half copy of R-O entries) and partially
   requires touching MI assumptions about `pmap_remove_all`.
5. **`icache_invalidate` granularity.**  We do a full-cache flush
   per `pmap_enter` on executable pages — if 51 page-faults all do
   this, it's a sizeable hidden cost.  A line-granularity invalidate
   would help, but probably needs RTL changes.

Previously listed as suspects but ruled out by measurement (do not
chase further without new data):

- `pmap_copy` over parent's full page table — already a no-op.
- `pmap_create` zero/copy cost — measured at 5.74 ms, fine.
- `cpu_lwp_fork` trapframe setup — 0.11 ms, negligible.
- `pmap_protect` "COW-marking pass" — 0.86 ms / 4 calls; NetBSD's
  COW is lazy, this pass doesn't exist.
- `pmap_kenter_pa` uarea cost — 66 µs / 1 call; uarea pool warms.

### What's not fixable in MD code (~46 % of total fork+wait time)

The `uvm_fault` exclusive bracket (~359 ms / fork) is MI internals:
`uvm_anon_alloc`, `uvm_pagealloc`, `amap_add`, trap-entry/exit
overhead, scheduler enqueue, struct proc/lwp setup, filedesc dup,
signal machinery, wait reaping.  Without forking the upstream
NetBSD tree, that's the floor we can't drop below.  Even an
arbitrarily-good MD layer can only halve the 670 ms or so.


## Hardware: UART hardware flow control (RTS/CTS)

With the 16-byte RX FIFO landed, paste loss only happens when an
inbound burst overruns the FIFO before the kernel can drain it — at
115200 baud the FIFO holds ~1.4 ms of data, and kernel ISR latency
tails (TLB miss chains during the paste itself, copyin from a long
`tty` queue update, etc.) occasionally exceed that on a 25 MHz CPU.
The proper fix is RTS/CTS hardware flow control: the FPGA tells the
host "stop sending" before the FIFO overflows.  Verified
2026-05-15 — the ULX3S board exposes the FTDI's modem-control
lines, but asymmetrically:

| Signal      | FTDI pin | FPGA pin | Direction         | Status                             |
|-------------|----------|----------|-------------------|------------------------------------|
| `ftdi_nrts` | RTS#     | M3       | FPGA reads        | wired, unconditionally available   |
| `ftdi_ndtr` | DTR#     | N1       | FPGA reads        | wired, unconditionally available   |
| `ftdi_txden`| TXDEN    | L3       | FPGA reads        | wired (RS-485 TX-enable hint)      |
| FTDI_nCTS   | CTS#     | V4       | **FPGA drives**   | **shared with JTAG_tdo**           |
| FTDI_nRI    | RI#      | R5       | FPGA reads        | shared with JTAG_tdi               |
| FTDI_nDSR   | DSR#     | T5       | FPGA reads        | shared with JTAG_tck               |
| FTDI_nDCD   | DCD#     | U5       | FPGA reads        | shared with JTAG_tms               |

(See the commented-out `LOCATE` block at the bottom of
`hw/constraints/ulx3s_v20.lpf` lines 229-232.)

The asymmetry is the critical detail: the half of flow control that
*matters for paste* — the FPGA telling the host to slow down — needs
the FPGA to drive `FTDI_nCTS` on pin V4, which doubles as
`JTAG_tdo`.  Using that pin requires:

1. Configuring the FTDI's MPSSE/D2XX side so the modem-control
   pins are routed to the UART channel rather than the JTAG channel
   (`fujprog` and ULX3S board design assume the JTAG mode at boot).
2. Either accepting that you can't reflash over JTAG while the
   FPGA is driving CTS (probably fine — `fujprog` resets the FTDI
   into JTAG mode before each flash), or wiring the pin tristate so
   the FPGA backs off during JTAG configuration.

The cheaper, JTAG-safe alternatives that *don't* fully solve the
problem but help:

- Bump `FIFO_DEPTH` from 16 to 64 (parameter is already exposed on
  `uart.sv`).  Buys 4× more buffering time, ~5.6 ms at 115200 baud
  — wide enough to absorb most kernel latency tails.  Caveat:
  com(4) detects the FIFO size from a separate IIR signature
  (TL16C750/16C950 variants) that we don't currently advertise;
  com(4) would still believe it's 16 deep and configure trigger=14
  accordingly.  Useful as breathing room but not a real fix.
- Lower the trigger threshold in com(4) (FCR[7:6]=00 → trigger=1)
  so the ISR fires on every byte instead of waiting for 14.
  Trades IRQ rate for headroom against kernel latency.

Independently of which mitigation lands, the underlying signal is
"kernel ISR latency tails > 1.4 ms occasionally."  That's worth
investigating in its own right (most likely a TLB-miss chain
during the very paste that's filling the FIFO, since the kernel's
input-processing path touches tty buffers and current LWP state).
Hardware flow control papers over the symptom but the latency tail
is its own diagnostic.

## RESOLVED: "uncached MMIO STW 2.4× slower than LDW" was codegen, not hardware

The hardware write path is symmetric — RTL-sim microbenches put a
single uncached STW and LDW at the same ~4 cyc/op.  The observed 2.4×
came from `-fno-strict-aliasing` (kernel-wide, `Makefile.kern.inc`):
with TBAA off the compiler can't prove a volatile MMIO store doesn't
alias the in-memory `bus_space_handle_t sc_ioh`, so it reloaded the
handle before every write — 3 instructions per "STW" vs 1 per LDW,
3:1 ≈ the observed 2.4×.  Do not re-investigate as a hardware
write-path asymmetry.

`pmci_burst` now uses the canonical `bus_space_{write,read,set}_multi_1`
primitives (handle passed by value → base pinned in a register, immune
to the reload; committed as `9ddfcf4aa941`).  Perf-neutral by design —
per-byte cost is dominated by the MMIO store latency, and the idiomatic
m68k flat-MMIO ports reach the same rolled-loop conclusion — the value
is idiom and immunity to the reload.

## Hardware: SD read throughput is bottlenecked by pmci_burst byte-PIO

SD reads run at ~190 KB/s through FFS ("loading a binary takes
seconds").  Root-caused by instrumentation on ULX3S @ 25 MHz CPU /
12.5 MHz SCLK (`time dd if=/netbsd of=/dev/null bs=32k` → 4.2 MB in
~22 s).  Per 512-byte block, split into card-wait vs our processing:

```
token poll (waiting on the card):   ~700 cyc   ~28 µs    ~2%
pmci_burst (our byte-PIO):        ~31,600 cyc  ~1.26 ms   ~98%
```

`pmci_burst` is ~45× the card wait and ~95% of the kernel `sys` time.
The ~1.26 ms is: fill 512 idle bytes (~287 µs CPU PIO) + clock 512
while busy-polling XFER_DONE (~328 µs) + drain 512 (~287 µs CPU PIO)
+ per-block CONTROL save/restore + FIFO flush.  All single-byte MMIO,
all sequential, bus idle during fill/drain.

**Falsified — do NOT re-investigate these:**
- the SD card (token-wait is negligible, ~28 µs/block);
- the SPI clock (wire is ~26% of the burst; the validated 6.25→12.5
  MHz bump gave only ~6%);
- transaction granularity (reads ARE coalesced to 64 KB CMD18 =
  128 blocks/cmd = MAXPHYS; FFS/buffer-cache cluster correctly);
- IRQ/cache/TLB noise (the earlier guesses here — `sys` is dominated
  by the byte-PIO, not interrupts).

**Levers, all in our control:**
1. Word-wide FIFO MMIO (4 B/access) — cuts the ~574 µs fill+drain
   PIO ~4×.  Needs HW FIFO-width + a CAP-tiered driver (byte path
   stays the boot/discrete floor; word path gated on a CAP bit).
2. Auto-idle TX for reads — let the engine clock 0xFF itself instead
   of the CPU filling 512 idle bytes; drops the ~287 µs fill.
3. Cross-block streaming — drain block N while the engine clocks
   N+1.  The 512-deep FIFO (= one block) forces today's stop-start.

Realistic: ~2-3× on the pmci term, ~1.5-2× end-to-end (the kernel
I/O stack + per-command framing is the other ~half of the run).  A
real SPI read-path + HW project, not a quick fix; gen2-adjacent.

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
floorplan (`ulx3s_penumbra2_floorplan.py`) corrals `u_sdram_ctrl` +
`u_sdram_cdc` into a compact block by the pads → ~117–127 MHz.

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
   the remaining words finish into BRAM in background).  Combined
   with critical-word-first burst ordering (already listed as
   Level 3 in `doc/internals/sdram-optimization.md`), the CPU sees
   just the bus round-trip latency rather than the full burst
   length.

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

## Libc: string-routine optimization — strlen word-at-a-time, strcmp/strcpy unrolled (DONE)

The shared string routines in
`common/lib/libc/arch/penumbra/string/{strcpy,strcmp,strlen,memcmp}.S`
are byte-at-a-time.  On Dhrystone they are not a minor cost — they are
most of the program.

Wiring the bare-metal benchmarks to these routines (replacing hand-rolled
byte-loop `string.c` helpers) moved gen2 HW Dhrystone 5.03 → 6.32 DMIPS,
entirely from fewer retired instructions — CPI was flat (3.03 → 3.06).
That win was the word-at-a-time `memcpy` alone: Dhrystone copies one
~48-byte `Rec_Type` per iteration as a `bl memcpy`, and byte → word
fill cut it ~288 → ~93 insns.  The L1-D store delta confirms it
(0.75 × 48 × iters of stores removed).  But the *remaining* per-iteration
count stayed high, which prompted a full attribution.

Instruction-count profile — deterministic, ISS-traced over 100
steady-state iterations (architectural counts, not timing): **727
insns/iter.**  Method is reusable and noted at the end.

| function                          | insns/iter | share |
|-----------------------------------|-----------:|------:|
| strcpy (byte loop, 6 insns/byte)  |      189.0 | 26.0% |
| strcmp (byte loop, 9 insns/byte)  |      178.0 | 24.5% |
| dhrystone_main (incl. inlined Proc_1–5) | 138.0 | 19.0% |
| memcpy (word loop, *called* from Proc_1/3) | 100.0 | 13.8% |
| Proc_8 (2-D array index)          |       62.0 |  8.5% |
| Proc_6                            |       22.0 |  3.0% |
| Func_2 / Proc_7 / Func_1          |       38.0 |  5.2% |

The three mem/string library routines are **64%** of every iteration;
`strcpy`+`strcmp` alone are **51%**.

The dynamic opcode mix shows the shape: `add` 157/iter (22%, mostly
pointer bumps — Penumbra has no post-increment addressing, so every
copy/compare step needs an explicit `add`); `ldb`+`stb` 109 (15%, byte
traffic); compare+branch (`test`/`cmp`/`bne`/`beq`/`b`) ~200 (28%, the
byte-loop control).  The genuinely ISA-structural tax these loops get
blamed on — 2-operand `mov` (49/iter) plus address materialization
`lli`+`lui` (40/iter, no gp-relative addressing) — is only ~12%, not the
main cost.

Reference point: a typical RV32 Dhrystone runs ~331 insns/iter, but it
links a libc whose strcpy/strcmp/memcpy are word-at-a-time and inlines
the small constant-size memcpy.  Most of the 2.2× gap is library
algorithm, wearing the costume of an ISA gap.

Outcome (HW-validated):

Word-at-a-time was tried for all three routines, then kept only for
`strlen`.  For `strcmp`/`strcpy` it *regressed* Dhrystone (750 vs 736
insns/iter): Penumbra has no unaligned access, so the word path needs
both operands co-aligned, and Dhrystone's are not — `strcmp(Str_1_Loc,
Str_2_Loc)` compares two adjacent `char[31]` stack arrays the compiler
packs at byte alignment, so the word path never triggers and the
routine just paid its setup (align prologue + two `LLI`+`LUI` constants
+ callee-saved spills) for a byte fallback.

Cross-platform practice settles it: the strict-alignment arches (mips,
m68k, sparc32) keep `strcmp`/`strcpy` as byte loops; only x86 word-
optimizes them, and only because cheap unaligned access lets it align
just one side.  Penumbra is in the strict-alignment camp.  `strlen`,
with one pointer and always-reachable alignment, is word-at-a-time on
nearly every optimized arch.

Resolution:
- `strlen` — word-at-a-time, frame deferred past the align prologue so
  a head-NUL returns without spilling.  Zero-byte lane located by
  shifting the detector mask's 0x80 to the sign bit (Penumbra has no
  CLZ; cf. sparc64's byte-wise mask scan).
- `strcmp`/`strcpy` — byte loops, unrolled 4× with offset addressing
  (m68k precedent), redundant trailing branch removed.

Real-HW results (gen2 FPGA):
- Dhrystone: 6.32 → 7.11 DMIPS (+12.5%), −242 cycles/iter, from the
  `strcmp`/`strcpy` unroll.
- `strlen` length sweep (pbench): +11% on len-8 (setup cost), but −30%
  at 64 B and −41% at 4 KiB.  Crossover ~len 12–16.

The instruction saving (−94/iter) and cycle saving (−242/iter) are
consistent across ISS, RTL sim, and HW; only the baseline differs.
Caution learned the hard way: the prebuilt RTL sim was stale (an older,
slower gen2 whose retire counter over-counts flushed slots, reading
~4× the instructions), and under-reported the win as −2.4% where HW
showed −10.7%.  Treat a sim as ground truth only if it is current; the
FPGA always is.

These optimizations *appreciate* on gen2.5.  The cycle saving is a
roughly fixed amount of removed work; gen2.5's lower flush/hazard
stalls shrink the baseline it is measured against, so the same saving
is a larger fraction.  `strlen`'s word loop is itself hazard-bound
today — the `haszero` chain (`sub`→`and`→`and`) is a dependent ALU
sequence an in-order scalar core cannot overlap, so it runs ~1.7×, not
the 4× the stride implies; that gap closes as gen2.5 hides hazards.

Correctness for all three is covered by a bare-metal test
(`benchmark/strtest`) over every start alignment × length.

Still open: inline constant-size `memcpy` in the backend.  Dhrystone's
48-byte `Rec_Type` copy lowers to `bl memcpy` (~100 insns/iter incl.
call + prologue); inlining ~12 word load/stores would drop it to ~25.
Complements the word-fast routine, which still serves the variable-size
case.

Profiling method (reusable): trace a short run with `+trace`, find a
loop-carried PC in the measurement function (exactly one hit per
iteration), then bucket trace PCs by symbol address-range between two
mid-run occurrences of that marker.  This isolates steady-state from
one-time setup/reporting and yields exact per-function and per-opcode
instruction attribution.

Related: "Libc: memcpy misses same-offset misaligned shortcut" and
"Libc: cache-line-align the memcpy/memset hot loops" tune the same
routine directory along the variable-size and code-placement axes.

## Libc: memcpy misses same-offset misaligned shortcut

NetBSD's libc memcpy on Penumbra takes the byte-fallback path for
*all* misalignment configurations, even the cases where a word-at-
a-time body would still work.  Measured via `pbench libc memcpy_align`
at n=256 (commit 3decf13fa4b0):

```
  aligned (size=256, main sweep):    ~83 us
  src=1, dst=0:                     ~287 us
  src=0, dst=1:                     ~287 us
  src=1, dst=1 (same offset):       ~290 us  ← could be word body
  src=1, dst=3 (different offsets): ~290 us  ← byte fallback only
```

When src and dst are misaligned by the same offset, after a 3-byte
head prologue both pointers reach word boundaries simultaneously
and the middle can run word-at-a-time.  Current libc misses this
case and lumps it with the genuinely-impossible different-offset
case.

Implementation lives in
`common/lib/libc/arch/penumbra/string/memcpy.S`.  An offset-
comparison check at the top would let the same-offset case fall
into the aligned word loop.

Expected win: ~3.4× on same-offset misaligned calls (~290 us → ~85 us
matching aligned).  Different-offset path is unchanged.  Affects
roughly half of all "misaligned" memcpy calls in practice (anything
where src and dst come from the same allocation pool or share a
base alignment).

## Libc: cache-line-align the memcpy/memset hot loops

The 2026-06-13 pbench snapshot showed the small-size `memcpy` rows
(size=1/16/64, and `memcpy_align` n=7/31) regressing while small-size
`memset` *improved* — across a pure compiler rebuild that touched no
libc source.  Diagnosis: the routines are hand-written assembly
(`common/lib/libc/arch/penumbra/string/{memcpy,memset}.S`), so their
instruction bytes are fixed; only their *placement* moved.  The L1
I-cache is 1 KiB **direct-mapped** with 16-byte (4-word) lines, and
neither `.S` carried any `.p2align` — so a rebuild relocates the fixed
routine bytes and the hot loops land at new, arbitrary offsets relative
to the line grid (and to whatever caller they share a set with).  The
memset-up / memcpy-down anti-correlation is the layout-roulette
signature.

Cross-arch check: penumbra was the *only* port declaring these routines
with bare `.globl`/`.type`, bypassing its own `ENTRY()` macro.  Peers get
at least the macro's entry alignment, and the hand-tuned ports (sparc64,
aarch64, sh3) explicitly `.p2align` their inner loops — sparc64 literally
comments `.align 32  ! ICache align.`.

Fixed in commit f0a9e9e0ec9a: `.p2align 4` on the function entries and
the hot loops — memcpy `.Lword` + `.Lbyte` (the byte loop is the whole
copy when operands are misaligned, so it is hot), memset `.Lword` only
(its `.Lhead`/`.Ltail_loop` run ≤3 times).  The loops now land on
16-byte boundaries at every build, so hot-loop placement is
deterministic and the small-size rows stop swinging on unrelated
code-size changes — consistency was the goal, restoring the benchmark's
power to detect real libc regressions.

HW result (min trial, vs the regressed 06-13 baseline): the fix recovers
the iteration-scaling part of the regression, and recovers *more* the
more the loop iterates — memcpy size=1 8.19→7.50 us (~23%), size=16
12.14→10.52 (~43%), memcpy_align n=7 11.90→10.51 (~38%), n=31
17.75→16.40 (~46%), n=127 41.25→39.95 (~full).  memset unchanged.  This
is the predicted split: `.p2align` removes the per-iteration line
straddle (so the win grows with trip count); the residual small-size
floor is the per-call **cross-routine set conflict** (routine vs. caller
congruent mod 1 KiB in the direct-mapped index), which `.p2align` pins
only the low 4 bits and so cannot touch — only associativity can (the
4-way L2, the gen2 cache direction).

Follow-up — the *compiler* has the same gap for compiler-generated code;
see "Compiler: align functions and hot loops in codegen" below.

## Compiler: align functions and hot loops in codegen

The Penumbra backend sets no `setMinFunctionAlignment`,
`setPrefFunctionAlignment`, or `setPrefLoopAlignment` (all default to
Align(1)), and nothing in MCAsmInfo.  Compiler-generated functions are
only *implicitly* 4-byte aligned (every instruction is 4 bytes); hot
loops are never aligned to the 16-byte (4-word) L1 I-cache line.  This is
the same straddle/placement problem fixed by hand for the libc string
routines (see "Libc: cache-line-align the memcpy/memset hot loops"), but
latent across all compiled code — kernel syscall loops, the qsort
comparator, etc.  Every comparable port sets min function alignment to
the instruction width (RISC-V/Mips/ARM/AArch64/Sparc = 4, AVR = 2); the
perf-tuned ones also set loop alignment (PowerPC = 16; RISC-V/ARM/AArch64
subtarget-driven).  Penumbra is the outlier that sets neither.

(a) `setMinFunctionAlignment(Align(4))` — hygiene, not perf.  Functions
are already 4-byte aligned because all instructions are 4 bytes, so this
emits a `.p2align 2` that produces no padding and leaves the binary
unchanged.  Worth doing only to make the intent explicit and match every
peer.

(b) `setPrefLoopAlignment(Align(16))` — the real lever, the codegen-wide
analog of the libc fix.  Notes from reading the LLVM machinery
(`MachineBlockPlacement::alignBlocks`):
  - Not blanket.  Only loop backedge destinations that the block-frequency
    model rates hot (≥ ~20% of entry frequency, and ≥ ~20% of their own
    loop header) are aligned; cold loops and optsize/minsize functions are
    skipped.
  - Smarter than the hand-written `.p2align`: it fills with NOPs but only
    aligns a loop when the padding lands off the hot path — in a dead gap
    after an unconditional-branch predecessor, or on a cold fall-through
    edge (loop rotation arranges most loops so this holds).  The libc `.S`
    routines bypass this path entirely, which is why they needed the blunt
    hand-pad and why this knob does not help them.
  - `MaxBytesForAlignment` defaults to 0 = *no cap*, so by default every
    qualifying loop pads to the full 16 bytes regardless of cost.  On the
    1 KiB **direct-mapped** I-cache (64 lines) that unbounded footprint can
    lose more to capacity/conflict misses than the straddle fix saves.
    Pair the knob with an override of `getMaxPermittedBytesForAlignment()`
    returning a small bound (≈4–8) so the assembler skips alignments that
    would need too much padding — align loops already nearly aligned,
    don't pay 15 bytes to move one.

Measure on hardware, do not assume: A/B with `qsort_int` and the kernel
syscall benches (the hot-loop-bound rows).  As with the libc fix, this
only addresses the intra-loop straddle; the cross-routine set-conflict
residual needs associativity.

## Compiler: support `[R0 + offset]` absolute addressing for low memory

`PenumbraTargetLowering::isLegalAddressingMode` currently rejects
`AddrMode` queries with `HasBaseReg = false` — that is, it tells
LSR / CodeGenPrepare that "just an offset" is not a legal address.
Strictly speaking it *is* legal: R0 is hardwired to zero, so
`LDW Rd, [R0 + offset]` reaches the low ±32 KB of memory in a
single instruction.  The kernel uses this for the trap vectors
(`0xFFFF_0000` is reachable as a negative offset from R0 thanks to
sign extension), and bare-metal MMIO probes commonly do
`*(volatile int *)0x100 = ...`.

The codegen pipeline does not currently materialize this form.  A
`G_LOAD` whose pointer is a small constant address goes through
the constant-materialization path (LLI / LLIS) and emits a real
`MOV` of the constant into a base register first.  Selection-side
work to recognise `(load (constant fits-in-simm16))` → `LDW Rd,
[R0 + offset]` would close this gap; once it does, drop the
`HasBaseReg` reject in `isLegalAddressingMode` so LSR knows the
mode is free.

Low-priority — the only places affected today are kernel
exception-vector reads and explicit MMIO accesses with very small
absolute addresses, both of which are written in inline asm or
hand-tuned C and don't go through optimization-sensitive paths.
But the discrepancy between "what the hardware can encode" and
"what the cost model claims" is worth closing for correctness of
optimization decisions in code we haven't yet seen.

## Compiler: signed sub-word loads in relational compares miss LDBS/LDHS

Re-measured and re-scoped 2026-06-22. The original framing (sext through
a `G_PHI`, mirroring `penumbra_zextload_promote`) is **superseded**:
upstream's `combines_for_extload` (in the post-legalizer list) now folds
`G_SEXT(G_LOAD)` → `G_SEXTLOAD` for the single-use, through-`G_PHI`
(including divergent-merge), and mixed sext+zext-consumer cases — all
verified emitting `LDBS`/`LDHS` (or `LDB` + a single mask) via `llc`. The
eq/ne char compares that were most of the 2026-06-09 Dhrystone finding
fold to plain `LDB`, so the Dhrystone binary now carries **zero** adjacent
sext chains.

The residual gap is narrower: a **signed relational** compare of a
sub-word load inside a loop. The canonical
`while (*p > 0 && *a == *b)` still emits `LDB; SHL r,24; SAR r,24` for the
`*p > 0` test (the `*a == *b` eq part folds to `LDB`) — the legalizer
widens the signed `G_ICMP` with `G_SEXT` and `combines_for_extload` does
not fold it to `G_SEXTLOAD` in that loop-carried shape, where the
isolated shapes do fold. Why the in-loop relational case bails when the
isolated cases fold is the thing to diagnose before writing any rule.

**Measured opportunity (static, register-checked: a `ldb`/`ldh`
immediately followed by a width-matched `shl;sar` on the same reg, which
`LDBS`/`LDHS` collapses 3→1):** kernel 41 (3 byte + 38 half), libc.so 20
(all byte), `sh` 0, Dhrystone 1. A lower bound (non-adjacent foldable
sites exist), but the immediately-adjacent count is small and ~2
instructions each — **low value**, consistent with the original "low
priority" tag. Not a hot-path lever; pick it up only if the relational
diagnosis turns out cheap, or if a kernel bench surfaces one of these in
a hot loop.

**Re-ranked 2026-09-13: the reach is in sign-bit tests, and it is
hot.**  The count above only matched a load immediately followed by
`shl; sar`.  Testing the sign bit of a byte or halfword (`b & 0x80`,
`(int8_t)b < 0`) emits the widened comparison against a materialized
−1, with other instructions in between:

```
ldb r3,[r1+1]; llis r4,-1; mov r11,r3; shl r11,24; sar r11,24; cmp r11,r4; ble
```

`ldb; test r3,128; bne` (or `ldbs; cmp r3,0; blt`) does the same in
three, often with one fewer callee-saved register.

- **SQLite:** 428 sites in `libsqlite3` (377 byte, 51 halfword) — one
  per varint and record-header byte, in `sqlite3GetVarint*`,
  `btreeParseCellPtr`, `sqlite3VdbeRecordCompare*` and
  `sqlite3VdbeExec`.
- **Doom:** the BSP walk tests `bspnum & NF_SUBSECTOR` on an i16
  child index as `llis; mov; shl 16; sar 16; cmp; bgt` where
  `test r1,#0x8000; beq` does it — once per node.
- **Kernel:** the `sy_narg` compare (`ldh; mov; shl 16; sar 16`) runs
  on every syscall.

A post-legalizer rule rewriting a sign test of `G_SEXT_INREG` into a
test of the sign bit covers the sign-bit cases.  The −1 is a separate
gap, in "compare and branch selection misses immediate and TEST
forms".  A related narrowing: `ldw r; sar r,16` with only the high
half used is `ldhs [+2]` — SelectionDAG narrows the load, GlobalISel
does not; 15 Doom sites, including `R_PointOnSide`.

## Compiler: selectAddSubCarry misses the immediate fold — RESOLVED

`selectAddSubCarry` emitted the register `ADD`/`ADC`/`SUB`/`SBC` for
the i64 carry chain whatever the second operand was.  Every 64-bit
counter bump — `cpu_nsyscall` on every syscall, `cpu_nintr` and the
`evcnt` counters on every interrupt, the TCP/IP statistics — carried
an `lli 1` for the low word and a `mov rz, zero` for the high one:

```
lli r4,1; mov r11,zero; add r4,r2; adc r3,r11
add r2,1; adc r3,zero
```

A small i64 subtraction was worse: `sub_to_add` turns `x - 1000` into
`x + -1000` before narrowing, giving `llis -1000; llis -1; add; adc`.

The constant now goes into the instruction when a form exists that
leaves the same result and flags: `ADDi`/`SUBi` for a uimm16 constant,
the opposite family's immediate for a negated one, `R0` for a zero
carry-in operand, and `R0` under the opposite opcode for an all-ones
one.  Shape coverage: `GlobalISel/select-add-sub-carry.mir`.

Measured statically on the hot-path review corpus (2026-09-13): kernel
−1,302 instructions (`.text` −5,208 B, 37 fewer callee-saved saves),
`libsqlite3` −450, Doom unchanged.  `syscall` −2, `intr_dispatch` −5,
`statclock` −14, `binuptime` −2.  The copied zeros feeding an `ADC`
went from 577 to 7 in the kernel.  Thirteen kernel functions grew, the
largest `ip_output` by 28 of 1,578 instructions, from the allocator
choosing differently once the constants' registers were gone.

## Compiler: redundant extension of comparison results

The branchless `icmp`-to-value path (`selectICmpToValue`) already
produces a clean 0/1 in a GPR, but the extension consumers re-process
it from scratch:

- `zext i1` -> `selectZExt` emits `ANDi #1`, a no-op on an already-0/1
  value.  The inverted predicates (`ult`, `ugt`, `ne`) compound it:
  `emitCarryToValue`'s own `ANDi #1` (masking the SBC's 0/0xFFFFFFFF down
  to 0/1) is then followed by the zext's `ANDi #1`, masking twice.
- `sext i1` -> `selectSExt` emits `SHLi #31` + `SARi #31` to turn 0/1
  into 0/-1, when the flag read could have produced the 0/-1 mask
  directly (a single SBC of zeros after the compare yields -(NOT C);
  pick the polarity to land 0/-1) and skip the two shifts.

The zext half is done: `selectZExt` emits a `COPY` (which the register
coalescer folds away) when the `G_ZEXT` source is a `G_ICMP`, dropping the
redundant mask.  Code in
`llvm/llvm/lib/Target/Penumbra/GISel/PenumbraInstructionSelector.cpp`.

Two pieces remain, both measured low-value 2026-06-06:

- **zext through a `G_PHI`.**  The `COPY` fold only fires when the icmp is
  the *direct* def, so `zext(phi(icmp, icmp))` still double-masks — 12
  cold kernel sites (all `and r,1; and r,1`).  Extending `selectZExt` to
  walk the PHI would have to prove every incoming value is 0/1, which is
  awkward at selection.  The idiomatic fix is instead a post-ISel
  `MachineFunctionPass` collapsing `ANDi x,C1; ANDi x,C2 -> ANDi x,C1&C2`
  (the redundancy only exists after selection, so a generic combiner
  cannot see it).  That is the same vehicle `RISCVOptWInstrs` (redundant
  `sext.w`) and `X86FixupSetCC` (zext-of-setcc) use for
  selection-introduced redundancy — but those target pervasive patterns;
  a pass for 12 cold instructions is not justified.  Reach for it if the
  pattern ever becomes frequent.

- **sext.**  `selectSExt` would have to *consume* the icmp (selection is
  bottom-up, and with no single-instruction negate, converting a 0/1
  afterward costs the same `SHLi;SARi` it would replace) and pick the
  carry polarity per predicate to land 0/-1 — a silent-miscompile
  surface.  Measured reach: 1 site in Dhrystone, 87 cold sites in the
  NetBSD kernel (0.23% of comparisons, ~0.02% of code).  Low leverage for
  the complexity; deferred.

Do **not** try to drive this from `setBooleanContents(ZeroOrOne)`.
Measured 2026-06-06: the redundant masks are emitted at *selection*,
after the combiners run, so boolean content cannot fold them.  Worse,
declaring `ZeroOrOne` flips `LegalizerHelper::lowerThreewayCompare` from
the SELECT-chain expansion to the arithmetic `zext(gt) - zext(lt)` form,
regressing the qsort i32 comparator (`scmp_i32` 10→15, `ucmp_i32` 10→13
instructions) while only shrinking the rarely-used i64 three-way compares
(−4 each); no other CodeGen test moves.  Only revisit `ZeroOrOne`
alongside a custom `G_SCMP`/`G_UCMP` lowering that keeps the i32
SELECT-chain.

## Compiler: constant multiply, divide and remainder strength reduction

`MUL` and `DIV` execute on the sequential `divmul` peer unit and hold
the core many times longer than an ALU operation — the per-core figures
are in [instruction-timing.md](internals/instruction-timing.md) and in
each core's scheduling model — so constant operands decompose into ALU
operations wherever that is cheaper on the tuned core.

### What exists

- **Multiply.**  `penumbra_mul_by_const` in the post-legalizer combiner
  rewrites an s32 multiply by a constant as shifts, adds and subtracts
  of the multiplicand.  `MulDecomposer` searches for the shortest
  sequence on the two-address machine, copies included: terms
  accumulated one at a time (Horner's rule, so the running sum is always
  the operand overwritten and a subtract costs no more than an add),
  factors of `2^n +/- 1` (`x * 0x01010101` is `257 * 65537`, 6
  instructions), a negative product built on `0 - x`, and a final
  `x - t` in the multiplicand's own register when the multiply is its
  last use.  `isMulDecompositionCheaper` takes the sequence where every
  instruction it adds over the constant load and `MUL` saves at least
  1.5 of the model's cycles, and at minsize where it is no longer.  A
  multiply whose operands also feed a `G_[SU]MULH` is left to fuse into
  one `MUL_P`/`MULU_P`.  The legalizer keeps only the s64 path (a 32x32->64
  widening multiply as one hardware multiply); s32 `G_MUL` is legal.
  Upstream's `mul_to_shl` / `mul_by_neg_one` cover powers of two and -1
  before legalization, at every optimization level.
- **Divide or remainder by a power of two.**  The legalizer lowers
  unsigned `G_UDIV` / `G_UREM` (so these reduce even at -O0); upstream's
  `sdiv_by_pow2` covers the signed divide, and `penumbra_srem_pow2` the
  signed remainder (see
  [the upstream-candidate write-up](llvm-gisel-signed-pow2-remainder.md)).
- **Divide or remainder by any other constant.**  Upstream's
  `*_by_const` magic-number rules, gated by `isIntDivCheap()`, which
  keeps the divide under minsize, above 32 bits, and where the tuned
  core's divide does not cost more than two multiplies plus a few ALU
  operations.  Narrow high-half multiplies from i8/i16 divides lower
  through a multiply at twice the width.

Covered by `mul-by-const.ll`, `div-by-const.ll`, `srem-pow2.ll`,
`strength-reduce.ll`, `mtune.ll` and the `penumbra-codegen/`
execution tests (`mul-const*.c`, `div-rem-const*.c`,
`sdiv-srem-pow2.c`).

### Known gaps

None of these produce wrong code.

- **The multiply cutoff's exchange rate is untuned.**  1.5 cycles per
  added instruction stands for the I-cache footprint: a miss costs a
  core the same cycles however fast its ALU operations are, so the price
  is absolute rather than a fraction of the multiply, but how often an
  added instruction misses depends on the workload's working set, which
  no single multiply sees.  At 1.5 penumbra1 accepts up to 12
  instructions (13 for a constant needing `LUI`; `x * 0x55555555` is
  12) and penumbra2_5 up to 5 (6).  Random 16-bit constants mostly need
  9-12 and random 32-bit ones 17-20, so the rate mostly decides small
  and structured constants.  Tune on the ULX3S, not the ISS; if the
  cores' measured I-cache miss penalties differ widely, the rate belongs
  to the subtarget.
- **-O0 keeps most constant multiplies.**  No post-legalizer combiner
  runs at -O0, so beyond upstream's power-of-two and -1 rules a constant
  multiply — `x * 3`, even `x * 0` — is a hardware `MUL` there.
- **One divide answer per type.**  `isIntDivCheap()` sees neither the
  opcode, the signedness nor the constant, so it prices every divide and
  remainder as the costlier remainder shape.  With today's cost points
  that decides identically to an exact answer.  An exact decision is a
  target rule that computes the sequence length from
  `UnsignedDivisionByConstantInfo` / `SignedDivisionByConstantInfo`,
  compares it against the model's latencies, and calls
  `CombinerHelper::applyUDivOrURemByConst` / `applySDivOrSRemByConst` —
  the hook then stays true.
- **64-bit divides keep their libcall.**  `G_SMULH` s64 does not
  legalize, and the hook cannot tell signed from unsigned.  `G_UMULH`
  s64 does (s32 partial products), so a per-instruction rule could take
  unsigned 64-bit divides away from `__udivdi3`; legalizing `G_SMULH`
  s64 unlocks the signed case.
- **A quotient and remainder of the same non-power-of-two divisor stay
  one `DIV_P`.**  `div_rem_to_divrem` fuses the pair and the magic rules
  match only the separate opcodes.  A rule shaped like
  `penumbra_srem_pow2` — quotient through the rewrite, remainder as
  `x - q * C` — would take it.
- **Narrow signed divide sequences carry redundant sign extensions.**
  The widened high-half product is sign-extended from its doubled width
  although it already fits, e.g. in `sdiv i8 %x, -3`.
- **A subtracting decomposition can cost an extra copy.**  A subtract
  does not commute, so its result stays in the register holding the
  shifted term, and that register cannot be the multiplicand's own
  while the multiplicand is live across the shift.  When the product
  is consumed elsewhere this costs nothing — `x * 7` stored to memory
  is `mov r3, r1; shl r3, 3; sub r3, r1` — but when it must land in
  the multiplicand's register (a return, or an operation tied there)
  a fourth instruction copies it back: returned, `x * 7` is
  `mov r2, r1; shl r2, 3; sub r2, r1; mov r1, r2`, where
  `mov r2, r1; shl r1, 3; sub r1, r2` is three.  The rule cannot fix
  this by emitting the copy itself: an explicit copy of the
  multiplicand ahead of the shift is coalesced straight back to the
  same four instructions.  It belongs to two-address lowering, which
  copies the shift's source rather than the subtract's other operand,
  or to the coalescer; a post-register-allocation peephole with a
  dead-register check would also catch it.  Every `2^n - 1` shape and
  subtracting constant is exposed (`* 7`, `* 240`, `* 255`), but the
  copy is rare in practice: across the compiled `make test-compiler`
  suite (~42M instructions) it appears only immediately before a
  return, 16 times, 12 of them in `penumbra-codegen/mul-const.c`
  itself, and never mid-function with the scratch register dead.
  That does not justify a new pass; revisit if more placement-level
  waste turns up.
- **The signed power-of-two remainder clears bits the long way.**
  `penumbra_srem_pow2` rounds the biased dividend down with `SAR` then
  `SHL`, because `-2^k` does not fit the uimm16 `ANDi` takes: 7
  instructions for `x % 8`.  Masking the remainder out instead keeps
  the bias live and needs `2^k - 1`, which fits for `k <= 16` —
  `((x + bias) & (2^k - 1)) - bias`, 6 instructions, and identical to C
  truncation for every `s32` including `INT_MIN` (checked for every `k`
  up to 16).  Above 16 the mask needs loading and the shift form stays
  shorter.  A fused quotient and remainder could share the bias.  The
  fix is local to `applySRemByPow2`.
- **Negation can take three instructions where two do.**  `0 - x`
  selects to `SUB` from a copy of R0, so when the result must land in
  the operand's register it is `mov r2, zero; sub r2, r1; mov r1, r2`,
  where `not r1, r1; add r1, 1` is two — and `NOT` takes a separate
  source, so the pair is two wherever the result lands.  The pair sets
  identical NZCV: SUB's carry is NOT borrow, so both set C only for
  zero and V only for `INT_MIN`.  The fix would be one selection
  pattern, `(sub 0, x)` → `ADDi (NOT x), 1`, but it is worth little.
  A survey of the rebuilt kernel, libc and 18 userland binaries (2.2M
  instructions) finds 687 negations, 0.03% of instructions, and the
  copy-back in only about 36 of them.  By first consumer, the
  negations are stored, copied, compared, passed to calls or unused
  nearby (386), masked as `x & -m` roundups with no cheaper form (79),
  or feed an add, directly or shifted (204).  Integer `a + -b` already
  folds in the IR, so the adds are C's `p - n`, which reaches the
  backend as `p + (0 - n)` and cost a zero copy, a subtract and an add,
  plus any scaling, where a subtract does.  The post-legalizer rule
  `penumbra_ptr_add_neg_to_sub` now rewrites it (see
  [the upstream-candidate write-up](llvm-gisel-pointer-minus-integer.md));
  the sites are per-packet mbuf adjustments in protocol input and output
  and driver ring pointers.  Negative constant multiplies do not need
  the pattern either: the decomposition accumulates from `0 - x` while
  the multiplicand is still live, so an in-place negation would not
  shorten `x * -80`.

### Cost model

`PenumbraTTIImpl::getArithmeticInstrCost` prices `Mul` and
`SDiv`/`UDiv`/`SRem`/`URem` in ALU operations — the tuned core's
multiply or divide cycles over its ALU cycles, rounded up — for
throughput and latency, scaled by the legalization factor, and leaves
code size to the base model: one instruction is emitted however long
it runs, and answering otherwise would distort inlining and unrolling.
Covered by `test/Analysis/CostModel/Penumbra/arith.ll`.  The cost model
corrects what LSR and the unroller see, but InstCombine canonicalises
without consulting it, which is why the multiply rewrite has to live in
the backend.

## Compiler: pr23135.c fails at -O0 (pre-existing, found in opt-level sweep) — RESOLVED

The 2026-06-09 full-suite sweep across opt levels (-O0/-O1/-O2 — the
routine `make test-compiler` gate only runs -O2) found exactly one
failure: `Regression/C/gcc-c-torture/execute/pr23135.c` (GCC
generic-vector arithmetic, `vector_size` attribute) exits 127 on the
ISS at **-O0 only**; -O1 and -O2 pass 1607/1607.  Bisected against the
Localizer change by rebuilding with the pass disabled — fails
identically, so it was pre-existing.  The earlier guess (vector
scalarization gap in `optnone_combines`) was wrong: it was the
dead-sum/live-carry selector bug — see "Compiler: two scalar
miscompiles surfaced by rebuilding compiler-rt — RESOLVED" above.
At -O0 nothing cleans up dead lo-half sums before selection, so the
scalarized i64 arithmetic hit the fusion hazard constantly.  Passes
with the fix.

## Compiler: codegen pass/gate audit — what we leave at the default

Audited 2026-06-09 after the shrink-wrapping discovery: Penumbra
overrides *no* behavior gates (only accessor plumbing in
`PenumbraSubtarget.h`), so several generic optimizations that mature
targets opt into are silently off.  Status per item:

- **GISel Localizer — DONE** (`4c016efc4222`).  Was missing from the
  pipeline entirely; AArch64 runs it at every opt level.  dhry_1.c
  static: stack refs 260→61, insns 816→670.  **HW-validated
  (ULX3S, 100K iterations): DMIPS 3.51→4.03 (+14.8%), CPI 4.29→3.74,
  at identical instruction count** — pure stall reduction: L1-I
  misses 77→44 per iteration (ifetch stalls 20.8%→13.6%), D-read
  misses 200K→597 total (spill reloads were nearly all of the cold
  D-reads), L2 read traffic −44%.  Post-change stall profile: ifetch
  13.6% and store 12.4% are the top terms — the store half is the
  write-through cost (L2 write-back / write-buffer hardware items),
  not compiler-addressable.
- **IRTranslator opt level — DONE.**  The translator was built at
  `CodeGenOptLevel::None`, so no branch probabilities reached block
  placement.  See "IRTranslator runs without branch probabilities".
- **Shrink-wrapping — DONE.**  `enableShrinkWrapping` defaulted to
  false.  See "shrink-wrapping (spill-density lever, part 1)".
- **Pre-RA MachineScheduler** (`enableMachineScheduler`, default
  false): with GlobalISel there is *no* scheduling at all without it —
  instruction order is IR order.  GenericScheduler in in-order mode
  (`MicroOpBufferSize=0`) prioritizes register pressure; flipping the
  hook also flips `enableJoinGlobalCopies` (cross-block copy
  coalescing), which defaults to `enableMachineScheduler()`.
  **Measured 2026-06-13, not pursued for gen1.**  Enablement is a
  one-line `enableMachineScheduler()` override (verified live: the pass
  enters the pipeline and the build is byte-identical to
  `-enable-misched=true`), but a static A/B is flat on gen1: `dhry_1`
  stays at 670 insns / 61 `[r14]` spill refs (≈40 lines merely
  reordered), and a deliberately high-pressure probe — 12 values live
  across a call — is unchanged at 56 insns / 26 spill refs.  No spill,
  instruction, or copy reduction.  Reason: on a single-issue, in-order,
  microcoded core there is no pipeline to schedule for, so the latency
  payoff is moot (`LoadLatency=1` is set precisely so the scheduler
  won't reorder ALU ahead of loads), leaving only register-pressure
  reduction — which the Localizer already captured on Dhrystone and
  which reordering cannot manufacture when spills are forced across a
  call.  The scheduler's real home is a pipelined generation; gate the
  enablement on the per-generation tuning work (the
  SchedMachineModel-per-processor mechanism in that entry below) rather
  than turning it on target-wide.  Revisit for gen1 only if a kernel
  pbench A/B surfaces reducible pressure that Dhrystone lacks.
- **Tail calls**: `PenumbraCallLowering.cpp` hardcodes
  `Info.IsTailCall = false` (`// TODO: tail calls`).  Wrapper-heavy
  kernel code pays a full frame per hop.  Measured opportunity and a
  small first-cut plan are in the dedicated "Compiler: tail-call
  optimization" entry below — ~5% of static call sites, a footprint +
  call-overhead win (not a spill win), ≈0 on Dhrystone.
- **`enableSpillageCopyElimination`** (MachineCopyPropagation
  extension, X86 enables): cheap flip + measure.
- **MachineOutliner / RISC-V-style save-restore millicode**: nothing
  implemented (`getOutliningCandidateInfo` etc.).  Code-size tools —
  attractive under the 1 KB I$ regime (21% of hot text is spill code;
  identical prologue/epilogue sequences outline well), but each
  outlined call costs BL+JMP at runtime.  Bigger project; consider
  RISC-V's `-msave-restore` shape (dedicated prologue/epilogue
  routines) rather than the general outliner first.
- **PostRA scheduler** (no Penumbra scheduling model sets
  `PostRAScheduler`): pointless for gen1 (single-cycle, no pipeline);
  belongs in a pipelined generation's own scheduling model (hide
  load-use and divmul latency).
- **MachineCombiner** (`getMachineCombinerPatterns` + sched model):
  reassociation for ILP — little to gain at IssueWidth=1; revisit
  with gen2.
- Not applicable: EarlyIfConversion (no predicated execution),
  MachinePipeliner (needs deep sched model, ILP machine),
  `enableSubRegLiveness` (no subregisters), GISel LoadStoreOpt
  (no wider load/store ops to merge into).
- **TTI capability/cost audit** (opened by the SDAG action-table
  removal): with no SelectionDAG path, IR-level passes read machine
  capabilities from `PenumbraTTIImpl`, and un-overridden defaults can
  answer wrongly in both directions.  `hasDivRemOp` — **DONE**
  (reports the fused DIV_P/DIVU_P; DivRemPairs keeps `x/y; x%y`
  pairs for the GISel divrem fusion instead of decomposing the rem
  into mul+sub — one ~33-cycle divmul op per pair instead of two).
  Remaining candidate: `getArithmeticInstrCost`, which prices
  MUL/DIV like single-cycle ALU ops for the inliner/unroller instead
  of ~33-cycle divmul iterations — a cheap override, but cost-model
  shifts move inlining decisions broadly, so it needs a kernel-bench
  A/B before landing.

## Compiler: shrink-wrapping (spill-density lever, part 1) — ENABLED

LLVM's ShrinkWrap pass (runs post-RA, just before PrologEpilogInserter)
computes a save point / restore point pair in `MachineFrameInfo` so PEI
places the *whole* prologue (CSR saves + stack adjust) at the earliest
block dominating all frame uses instead of the entry block — early-exit
paths then run zero frame code.  It is gated per-target on
`TargetFrameLowering::enableShrinkWrapping`; every major target
(AArch64, ARM, LoongArch, Mips, PowerPC, RISC-V, X86) opts in, and
`PenumbraFrameLowering` now does.

Two correctness problems had to go first.  The force-flag trials
(`-enable-shrink-wrap=true`, 1607/1607 on the compiler suite in June)
never reached either shape:

- **Epilogue placement.**  `emitEpilogue` released SP before the
  block's *last* instruction, and PEI places the callee-saved reloads
  before its *first terminator*.  In a return block those coincide.  A
  restore point ending in `bge; b` released SP on the fall-through path
  only, and one falling through with no terminator released it before
  the last reload, which then read the wrong slot.  Two kernel files
  also failed to assemble.  The release now goes before the first
  terminator, after the reloads.
- **Clobbered state.**  `canUseAsPrologue`/`canUseAsEpilogue` refuse a
  block where the SP adjustment overwrites a value still read: SR
  between a compare and its branch — every ALU op sets NZCV and the
  ISA has no flag-preserving add, so X86's `LEA` escape does not exist
  — or R11 when the frame may need the register form of `adjustSP`.
  Liveness comes from `LiveRegUnits` over the successors' live-in
  lists.  SR is block-local by construction today; code that ever
  splits a block while flags are live must add the SR live-in itself,
  as X86's select lowering does for EFLAGS.  A refused restore point
  that is the only candidate leaves the frame in the entry block.

Shape coverage: `shrink-wrap.ll`.

Measured statically on the IR corpus of the hot-path review
(2026-09-13), the same `llc` with and without
`-enable-shrink-wrap=false`:

| | framed functions | shrink-wrapped | `.text` |
|---|---:|---:|---:|
| Doom | 928 | 67 (7.2%) | −12 B |
| kernel `GENERIC.DEBUG` | 5,881 | 230 (3.9%) | +188 B |
| SQLite (PIC) | 1,642 | 101 (6.2%) | +184 B |

The hooks refuse little: the pre-fix compiler with the force flag
shrink-wraps two more Doom functions and the same number elsewhere.
SQLite's `getCellInfo` (cached path 14 instructions → 4) is among the
shrink-wrapped.  Dhrystone's hot functions have no early-exit shape;
kernel error paths and lock fast paths are where it can show.

Open:

1. **Reach limit — regalloc pins the save point for argument values.**
   An argument live across calls gets its `COPY` into a callee-saved
   register placed in the *entry* block, which ShrinkWrap counts as a
   frame use, so such functions never shrink-wrap.  The standard
   counter is `TargetRegisterInfo::getCSRFirstUseCost` (AArch64 returns
   5), which makes greedy RA pre-split cold CSR first-uses — but
   forcing `-regalloc-csr-first-time-cost` did *not* split the argument
   shape on Penumbra (the region-split candidate search found nothing
   under CSRCost).  Needs its own investigation; it is the likely cause
   of the low kernel count above.
2. **`R_PointOnSide`** still saves 4 registers on its axis-aligned
   early exits, which riscv32 shrink-wraps with 2 — 52 cycles per exit
   on penumbra1, 26 on penumbra2_5.  Not root-caused; the six returns
   sharing one return block, or `r5` used as scratch before the first
   early exit, are the suspects.  Doom's BSP leaf visits also need the
   tail-call entry.
3. **Known regression, accepted — DDB backtraces.**  The kernel
   unwinder (`db_trace.c`) finds a frame by scanning the first 16
   instructions of the function for `SUB r14,#imm` and the `STW r13`
   save, stopping at the first instruction outside that pattern.  A
   shrink-wrapped function starts with its early-exit test, so the
   scan finds no frame and treats the function as a leaf: the walk
   stops at the first shrink-wrapped caller, and a trapping
   shrink-wrapped frame past its prologue unwinds with a stale LR and
   the wrong SP.

   Other ports: x86 and AArch64 build the kernel with frame pointers
   and walk the chain, which shrink-wrapping cannot break above the
   trapping frame.  MIPS and SH-3 scan forward from the symbol like
   Penumbra and break the same way; MIPS stops at the first branch and
   says so ("return breaks stackframe-size heuristics with gcc -O2").
   Alpha's `decode_prologue` is the one written for moved code: it
   scans every instruction from the symbol to the PC, adds up the stack
   allocations wherever they sit and ignores stack releases, since an
   epilogue can be laid out before a PC still inside the frame.

   The fix, when backtraces matter enough: scan Alpha-style.  Caller
   frames are then always right — a return address lies inside the
   saved region, because a call needs LR saved.  The trapping frame
   stays a guess (the trap may sit before the saves or past the
   restores); check that the LR slot holds a kernel text address
   following a `BL`/`JALR`, and otherwise fall back to the live LR and
   the unadjusted SP.  A frame pointer in R10 would cost a save and an
   allocatable register in every framed function, the cost
   shrink-wrapping exists to remove.

## Compiler: tail-call optimization — measured opportunity

`PenumbraCallLowering::lowerCall` hardcodes `Info.IsTailCall = false`
(`// TODO: tail calls`), so every call — including a pure forwarding
wrapper `return bar(args)` — emits a full `BL`/`JALR`, a frame, and a
`jmp lr` return hop.  Every major GISel target lowers tail calls;
Penumbra is missing the wiring, not the capability.

### Static opportunity (measured 2026-06-22)

Method: static scan of `llvm-objdump -d` over the real built binaries.
A *tail-position call* is a `bl`/`jalr` whose only successors before the
function's terminal `jmp lr` are frame teardown — a `ldw` of a
callee-saved reg or `lr` from `[sp+N]`, plus the `sp` adjust — so the
result in r1/r2 passes through untouched.  Restricting teardown-loads to
callee-saved targets (excluding a `ldw r1,[sp]`, which would mean a
reloaded local rather than the call result) moved every count by <2%, so
the heuristic is tight.

| binary | total calls | tail-position | % | free-frame wrappers |
|--------|------------:|--------------:|--:|--------------------:|
| kernel (MINIMAL) | 37,267 | 2,027 (1,869 `bl` + 158 `jalr`) | 5.4% | 531 (28% of direct) |
| libc.so.12       | 20,113 |   937 (875 + 62)                | 4.6% | 476 (54% of direct) |
| bin/sh           |  2,986 |   105                           | 3.5% |  23 |
| bin/cat          |     62 |     2                           |   —  |   2 |

So ~4–5% of all call sites already compile to a tail-position call.  The
"free-frame wrapper" column is the subset whose stack frame exists
*solely* to save `lr` across the call; libc's forwarders (`__foo`→`foo`
weak aliases) make it over half of libc's tail sites.

### Per-site savings — two tiers

- **Plain tail call**: −1 static instruction (the trailing `jmp lr`
  disappears; `bl`→`b`, `jalr`→`jmp`), and −1 dynamic taken
  control-transfer — the callee returns straight to our caller instead
  of bouncing back through our frame.
- **Free-frame wrapper**: the whole prologue/epilogue collapses to a
  single `b target`, removing a **write-through `stw lr`** + its reload
  *per dynamic execution*.  Given gen1 store-stalls are ~12% of cycles
  (see the "Dhrystone hot-path code shape" entry below), that is real on
  a hot wrapper, not cosmetic.

### Expected leverage — footprint + call-overhead, not spill

Workload-shaped, unlike the broad spill win the Localizer captured:

- **Dhrystone / compute kernels: ≈0.**  Their hot loops carry no tail
  calls — the same structural reason shrink-wrapping measured flat.  Do
  not expect DMIPS to move.
- **Wrapper-heavy paths: modest but real.**  Kernel syscall dispatch,
  VFS/VOP indirect dispatch (the 158 `jalr`-tail sites are `VOP_*`-
  shaped), libc forwarders, and process startup (ties into the "fork()
  is unreasonably slow" entry).  Estimate: low single-digit % on
  call-overhead-bound benches (pbench syscall rows), not a
  Localizer-sized number.
- **Recursive tree walks.**  Doom's `R_RenderBSPNode` pays its full
  7-register frame on every leaf visit, which only masks the child
  index and calls `R_Subsector`.  With tail calls and shrink-wrapping
  a leaf is `test; bne; mask; b R_Subsector` — about 90 cycles less
  per subsector on penumbra1, 50 on penumbra2_5.  SQLite's
  `sqlite3BtreeNext` keeps a frame only to save `lr` around a
  tail-position `bl`.
- **Code size: ~−2K kernel instructions at the −1/site floor,
  ~−15–18 KB once free-wrapper frame removal counts** — helps the 1 KB
  direct-mapped gen1 I$ only where the wrapper text is hot.

A hard percentage needs a **dynamic** call-site histogram: run a
wrapper-heavy workload (kernel pbench syscalls / fork+exec) under the ISS
with call tracing and weight these static sites by execution frequency.
Dhrystone provably cannot resolve this — measure on the syscall/startup
benches.

### Implementation — a small first cut exists

The audit's "medium GISel project" framing is the *fully general*
version; a useful first cut is small:

- **First cut**: tail-call only when there are **no outgoing stack args**
  (≤4 register-arg-words) and the calling conventions match.  Reuse the
  existing `PenumbraOutgoingValueHandler`; emit a `TAILBL`/`TAILJMP`
  terminator pseudo instead of `BL`/`JALR`; let PEI sink the epilogue
  before it.  Penumbra's caller-pop ABI means no callee-cleanup
  mismatch — a real simplifier.  The tail branch stays in the same
  privilege level (SSP/USP banking untouched), R12/curlwp is reserved,
  and `lr` is restored before the branch so the callee returns to our
  caller.  This already catches most free wrappers, which are typically
  ≤4 args.
- **The medium tail**: outgoing stack args overlapping the incoming arg
  area (ordering-sensitive copy), `sret`, varargs, and the
  eligibility-predicate corners.  A minority of the opportunity.

RISC-V's `lowerTailCall` + `PseudoTAIL`/indirect-tail pseudo is the
matching template; AArch64's `isEligibleForTailCallOptimization` is the
eligibility-check reference.

## Compiler/benchmark: Dhrystone hot-path code shape — where the cycles go

Measured 2026-06-09 (PC-weighted ISS trace histogram over 300 iterations
+ disassembly review; method: `+trace=` through a FIFO into a per-PC
`awk` histogram, symbolized against the ELF).  Numbers are for the
`benchmark/` bare-metal Dhrystone at -O2, gen1 geometry (1 KB
direct-mapped L1-I, 16 B lines).

### The headline: I-footprint, not instruction count

- **928 dynamic instructions per iteration** (ISS, CPI=1 by
  construction; the ISS-ideal bound is 13.2 DMIPS @ 25 MHz, so
  HW-DMIPS / 13.2 is the effective stall multiplier).
- Per-iteration hot text: **81 distinct 16 B lines (1296 B) vs the
  64-line (1 KB) direct-mapped I$**.  30 of 64 sets hold more than one
  hot line; ~70 of the 81 lines share a set with another line touched
  in the same iteration.  A cyclic sweep through that working set
  conflict/capacity-misses nearly every line, every iteration —
  expect L1-I read misses/iteration in the ~70–80 range on hardware.
  (Confirmed: 77/iteration measured on the ULX3S pre-Localizer;
  44/iteration after — see the pass/gate audit entry.)
- Instruction-count micro-optimizations are invisible under this:
  the select-CMPi fold (commit `8a23e2222632`) measured flat on HW
  Dhrystone for exactly this reason.

### Dynamic instruction shares (per iteration)

| code | insns/iter | share |
|------|-----------:|------:|
| memcpy (2× 48 B struct assign) | 292 | 26% |
| strcpy | 189 | 17% |
| strcmp | 165 | 15% |
| main loop body (Proc_1–5, Func_3 inlined) | 131 | 12% |
| Proc_8 | 64 | 6% |
| Proc_6/7, Func_1/2 | ~95 | 9% |

The string routines are 58% of dynamic instructions from only ~10
I$ lines — they are the cache-*friendly* part (tight pointer-bump
loops).  The other ~40% sweeps the ~70-line Proc/loop text.

### Findings, ranked by leverage

1. **Spill/reload density is the top compiler lever.**  21% of hot
   static text is `stw`/`ldw` against `[r14 + N]` (callee-save
   prologues + locals reloaded across calls), another 14% is `mov`
   shuffles (two-address tax).  The loop body alone is 39% loads.
   Examples: `Proc_8` saves 6 callee regs for straight-line address
   arithmetic; `Proc_1` saves 7.  This matters doubly because spill
   *stores* are write-through all the way out on gen1 — and gen2
   keeps write-through L1 at least initially, so spill cost carries
   forward.  Candidate directions: shrink-wrapping, register-pressure
   aware address-arithmetic scheduling/CSE, revisiting CSR allocation
   order.
2. **Word-wise string routines** (bare-metal `benchmark/common` and
   NetBSD libc both): rolled byte/word loops cost ~150–200
   insns/iteration here for ~+10 lines of shared footprint.  Related:
   the existing "Libc: memcpy misses same-offset misaligned shortcut"
   entry.
3. **I$ geometry is a gen2 matter.**  gen1 2-way associativity was
   tried and **blew timing** (the fetch/TLB path is already the fmax
   limiter) — treat gen1 I$ improvements as infeasible; gen2's BRAM
   VIPT 4-way design (Decision 11) is the fix.
4. **Branchless verbosity is real but small in Dhrystone**: exactly
   one hot site (Proc_6's 8-insn `sext(eq)+add` select-of-constants
   chain, see the branch-cost entry below).  Kernel hot paths may
   differ — measure there before investing.
5. **Signed sub-word loads — the Dhrystone instance is now gone
   (re-measured 2026-06-22).** The 2026-06-09 profile saw ~6
   `shl 24; sar 24` sext-of-char chains in the loop body; the eq/ne
   char-compare cases since fold to plain `LDB` and the rest to `LDBS`,
   so the Dhrystone binary now carries **zero** adjacent sext chains
   (1 `ldb+shl24+sar24` site total, not on the hot path). The remaining
   gap is the *signed-relational* shape (`while (*p > 0)`), not eq/ne —
   see the "signed sub-word loads in relational compares" entry, now
   re-scoped and measured small.
6. Minor: `Proc_1`–`Proc_5`/`Func_3` are fully inlined but their
   out-of-line bodies stay linked (extern linkage) — ~500 B of dead
   text between hot functions.  Harmless to the cache (never fetched)
   but skews naive footprint reading; `--gc-sections` +
   `-ffunction-sections` would drop them.

## Compiler: hot-path code review — kernel, SQLite and Doom

Reviewed 2026-09-13: the `GENERIC.DEBUG` kernel, `libsqlite3` (PIC) and
a Doom rebuild, all from the same compiler at the default tuning.
Doom's hot functions came out the same under `-mcpu=penumbra1` apart
from constant multiplies.  All counts are static disassembly scans by
throwaway scripts.  Nothing is weighted by execution frequency yet;
for Doom, the renderer counters of a `PERFCTR=1` run supply the
weights.

Compiler causes, widest reach first, each with its own entry:

| Cause | Shows up in | Entry |
|---|---|---|
| No branch probabilities reach block placement | every `__predict_false`, KASSERT and cold path | "IRTranslator runs without branch probabilities" |
| Countdown loop exits test a materialized overflow flag — RESOLVED | Doom column and span drawers: 6 of 18 instructions per pixel | "countdown loop exits pay for an overflow flag" |
| s64 relational compares | SQLite b-tree bisection and record compare | "64-bit relational compares expand to select diamonds" |
| Compare immediates, TEST, inverted booleans | 848 / 612 / 274 / 140 sites | "compare and branch selection misses immediate and TEST forms" |
| Byte and halfword sign-bit tests | SQLite varint bytes, BSP child index, `sy_narg` | "signed sub-word loads in relational compares miss LDBS/LDHS" |
| Constants never rematerialized | 220 kernel loops; ~1,250 SQLite constant reloads | "constants are not rematerializable" |
| `&global + C` built in the entry block; addresses hoisted into callee-saved registers | 1,282 SQLite sites; Doom BSP walk frame | "constant-offset global addresses stay in the entry block", "early MachineLICM hoists global addresses into callee-saved registers" |
| `p - K` and integer MMIO addresses miss the offset fold | memmove backward loops; 187 driver sites | "pointer-minus-constant and integer MMIO addresses miss their folds" |
| 64-bit counter increments — RESOLVED | every syscall and interrupt | "selectAddSubCarry misses the immediate fold" |
| A store between producer and compare | `KPREEMPT_ENABLE` on every `userret` | "compare-elim does not look through COPY" |

Most of the frame cost belongs to the shrink-wrapping and tail-call
entries.  Shrink-wrapping is enabled, but its reach is limited — see
item 1 of its open list.

### Tree walks

Doom's BSP walk is the first tree-walking code studied here.  Its data
access is already close to minimal — the node loads and `nodes[]`
indexing leave nothing to take.  The cost per node is the call:

- The recursion saves 7 callee-saved registers per level, leaves
  included (tail-call and shrink-wrapping entries).
- Early MachineLICM moves the `viewx`/`viewy` address loads into
  callee-saved registers: two more saves per call (early MachineLICM
  entry).
- The `NF_SUBSECTOR` test on the i16 child index is 6 instructions for
  a 2-instruction bit test (sub-word entry).
- The near/far child order and early exits are laid out without
  probabilities.
- `R_PointOnSide` keeps its frame on its early exits, and `FixedMul`
  and `R_PointToAngle` are calls into other files ("Doom: renderer
  source-level costs").

A tree walk needs cheaper calls more than better expression code: fewer
saved registers, and recomputation preferred over a callee-saved
register for anything one or two instructions long.

### Smaller findings

- **Pointer copy in scan loops.**  The `solidsegs` scans in
  `R_CheckBBox`, `R_ClipSolidWallSegment` and `R_ClipPassWallSegment`
  use the pointer after the loop, so the tied `ADD` needs a copy every
  iteration: `mov; ldw; add; cmp; blt`, one instruction more than
  necessary.  43 small loops in Doom.
- **MachineCSE leaves same-block duplicates.**  `R_MapPlane`
  recomputes `y << 2` for each parallel array.  On a MIR repro,
  `-run-pass=machine-cse` merges duplicate address pseudos but not
  `SHLi`, `NOT` or `LLIS`, even with `-aggressive-machine-cse`.  The
  rejecting check is not identified yet (needs an assertions build).
  17 Doom sites.
- **Cancelling adds.**  `sqlite3VdbeExec`'s `jump_to_p2` does
  `llis r7,-20; add` on the way in and `add r7,20` at the shared tail
  (34 branches in); riscv32 does the same.  98 cancelling or mergeable
  pairs in `libsqlite3` — a post-RA constant add merge would catch
  them.
- **Copy churn.**  75 `mov a,b; mov b,a` pairs at join points and 30
  duplicate `LLI`s (`sqlite3VdbeSerialGet`: `lli r1,4; sth; lli r1,4`)
  in `libsqlite3`.

Checked and not levers:
- SQLite's big-endian `get4byte` decode and its ×40 `aMem[]` scaling
  are already minimal.
- `pcache1` divides by a run-time `nHash`.
- Doom's `(frac >> 16) & 127` column index is already minimal as
  compiled.  Keeping the fraction pre-shifted in the source removes
  the `AND` — see "Doom: FastDoom techniques that transfer".
- SQLite's VDBE dispatch — 13 instructions per opcode, including an
  `STW` of `nVmStep` for lack of registers — is PIC jump-table cost
  plus register pressure.  The bounds check could only go by padding
  the table to 256 entries.

Source-level findings from the same review are in "Kernel: source-level
costs on the trap, syscall and driver paths" and "Doom: renderer
source-level costs".

## Kernel: source-level costs on the trap, syscall and driver paths

Found in the hot-path code review (2026-09-13); none of these is a
code-generation problem.

- **`copyin`/`copyout` reload curlwp.**  `GET_PCB` in `copy.S` loads
  `cpu_info_store.ci_curlwp` (`la` plus `ldw`, 3 instructions),
  although R12 holds curlwp whenever kernel code runs;
  `ldw reg, [r12 + L_PCB]` is one.  Both routines also save `r6` only
  to hold `VM_MAXUSER_ADDR` for one compare, where the unused
  caller-saved `r4` would do.
- **`trap()` builds a 168-byte, 7-register frame on every trap.**  The
  signal-delivery and fault-handling cases share the frame with the
  common return path.  Moving them into `noinline` helpers leaves the
  hot path a small frame.
- **Trap entry saves registers twice.**  The vector stubs and
  `_trap_common` park R1–R4 in PC-relative scratch memory, then copy
  them into the trapframe — roughly 10 extra memory operations per
  trap.  Whether the general path can save straight into the trapframe
  or use the scratch SPRs, as the TLB-miss handler does, is the
  question to answer first.
- **No mutex stubs.**  `mutex.h` defines `__HAVE_SIMPLE_MUTEXES` but
  not `__HAVE_MUTEX_STUBS`, so every `mutex_enter`/`mutex_exit` is a
  call into MI `kern_mutex.c`.  A stub needs an atomic compare-and-swap
  of its own, so measure the call's share before designing one.
- **com(4) register access is an indirect call.**  MI `com.c` routes
  every register read and write through the `cr_read`/`cr_write`
  pointers `com_init_regs` installs.  That is the MI design; avoiding it
  means a different UART driver, not an MD change.
- **`pdisplay` scrolling reloads bases per cell.**  The row and column
  copy and erase paths reload their base pointers for every character
  cell: 10 instructions per cell where 6 do.  Hoisting them into
  locals, as `b82fb4b0df29` did for Doom's column drawer, fixes it.

## Kernel: pack the hottest syscall-path text onto shared pages

Low priority — syscall performance is already good; this is a future
refinement, not a fix.

Kernel text is mapped through the main TLB: 32 sets of 2 ways, set =
the low 5 bits of the VPN, victim way chosen from a `CPU_CYCLES` bit.
A miss runs the software handler (about 59 instructions to `eret`)
and counts as benchmark instructions.  The trap entry already lives on
the pinned vector page, but everything after it is wherever the link
order put it.  `clock_gettime(CLOCK_MONOTONIC)` alone touches seven
text pages spread across the image — `trap` and `timer_get_timecount`
near `0x80014000`, `syscall` and `copyout` in
`0x80015000`–`0x80017000`, `nanouptime`/`binuptime` and
`sys___clock_gettime50` in `0x801a6000`–`0x801ac000`,
`clock_gettime1` at `0x801e3000`, `memcpy` at `0x802d0000` — plus
data pages (`cpu_info_store`, `sysent`,
`emul_netbsd`, `timehands`).  Any set holding three per-call pages
misses on every call, and which sets fill up moves with unrelated
layout changes.  The IRTranslator probability change showed this:
`clock_gettime` retired more instructions per call on hardware while
every function on its path came out the same length or shorter,
consistent with a TLB set filling up differently.

The idea: place the common syscall path (`trap`, `syscall`, `userret`,
`copyin`/`copyout`, `memcpy`) and the hottest handlers
(`clock_gettime`, `getpid`, `read`/`write`, the timecounter read)
contiguously, so the per-call path fits in one or two pages; likewise
their hot data.  The kernel builds without `-ffunction-sections` and
links with `kern.ldscript`, so this needs per-function sections plus
an ordering — a symbol ordering file for lld, or a `.text.hot` input
section the script places first.

Measuring it needs a TLB miss count, which nothing exposes today (see
"Kernel: count TLB refills"), and a before/after on the pbench
syscall rows.

## Kernel: count TLB refills

Nothing counts refills.  `_real_miss_handler` resolves them without
entering `trap()`, so neither `vmstat -s` nor an evcnt sees them, and
the CPU perfctrs count the handler's ~59 instructions as instructions
of whatever was running.  The TLB's shape, and why its misses move
with layout, are in "Kernel: pack the hottest syscall-path text onto
shared pages".

Doom makes the case.  Moving its lumps out of the zone removed ~1M
instructions a frame, a fifth of the total, with the game's own work
unchanged, and an unrelated change had already moved ~0.3M ("Doom:
where a frame goes").  Refills are the one per-access kernel cost
that fits.  Reference-bit emulation is ruled out:
`pmap_clear_reference` only clears the software flag.

A counter word in pinned scratch, bumped on the fast path and exported
through sysctl beside the perfctrs, settles it.  Splitting it by the
faulting VA's region (text, heap, mapped file, stack) would say which
data to move.

## Doom: where a frame goes

Measured 2026-09-18 on hardware at 37.5 MHz: a `PERFCTR=1` build
playing the 150-tic `BENCH.lmp` timedemo of Freedoom's E1M1.  By feel
a 386DX/40 runs Doom 4–5× faster.

| | lumps read into a 16 MB zone | WAD mapped, 4 MB zone |
|---|---:|---:|
| fps | 2.52 | 3.03 |
| instructions / frame | 5.02M | 4.03M |
| cycles / frame | 14.8M | 12.3M |
| CPI | 2.95 | 3.05 |

The mapped run's stalls: load 21%, store 14%, ifetch 14%, flush 11%,
hazard 4%, funit 3%; hit rates L1I 97.2%, L1D 82.4%, L2 86.8%.

- **The instruction set is not the gap.**  Host gcc 11 at `-m32
  -march=i386 -O2` (compile-only, with an empty `gnu/stubs-32.h` and a
  `sys/endian.h` shim) against our clang `-O2` over the eight renderer
  files: 6,151 instructions against 9,026, the usual CISC/RISC ratio.
  The pixel loops are within 1.2× — `R_DrawColumn` 10 against 12 per
  pixel, `R_DrawSpan` 15 against 17, the difference being x86's
  base+index addressing — and with every access hitting, the timing
  tables put Penumbra ahead per clock: ~19 cycles a column pixel on
  penumbra2_5 against ~35 for the i386 C loop.  Outliers:
  `R_RenderSegLoop` 201 against 405, 139 of them `lli`/`lui` pairs for
  globals (`LUI` ORs, so there is no `%hi`/`%lo` fold into the load);
  `R_DrawVisSprite` 84 against 242, not yet examined.
- **The pixel loops are ~16% of the instructions.**  46.5k pixels a
  frame at 12 and 17 instructions come to 0.65M of 4.0M.  A column
  call draws 7.2 pixels on average (29,058 in 4,014 calls), so
  per-call setup outweighs the pixels — see "Doom: renderer
  source-level costs".
- **The BSP walk is not a lever.**  LLVM already turns the back-side
  recursion into a loop, and the front-side descent's 7-register frame
  is at most ~3.8k memory operations a frame, ~0.1%.  The traversal
  proper — `R_AddLine`, `R_CheckBBox`, `R_FindPlane`'s linear scan of
  the visplanes, the node visits — is an estimated ~0.2M instructions.
  The `bsp` phase is big because every wall runs inside it: only one
  `R_StoreWallRange` call in 16 is bracketed as `wall`.
- **Part of the frame is not Doom's.**  The per-frame work counts are
  identical in every run, yet instructions fell from 5,025k to 4,719k
  after a change that touched no frame code (unbuffered stdout), and
  to 4,029k once the lumps left the zone.  That second drop sits in
  the sprite (−570k) and plane (−171k) phases, whose data moved; a
  time-uniform cost would have come mostly out of `bsp`.  Skipping
  `Z_ChangeTag2` accounts for ~60k.  TLB refills are the suspect — see
  "Kernel: count TLB refills".  Until that is understood, one layout's
  runs vary by ~6%: judge small changes over several runs.

`r_phase` gaps:

- Every mark is a `sysctl` call.  A sampled wall call carries two, and
  the scaled `wall` estimate (7.8M cycles) exceeds the whole bsp+wall
  bucket (6.9M), so a mark pair costs at least ~15k cycles.  Calibrate
  it with a back-to-back pair and subtract before trusting the
  bsp/wall split.
- Only cycles and instructions are kept per phase, although `read_cpu`
  fetches all eight counters.  Per-phase stall buckets would say which
  phases are load- or store-bound.
- The whole-run `perf_report` line subtracts a single snapshot pair of
  32-bit counters, so it is meaningless past 2^32 cycles (~115 s at
  37.5 MHz).  `BENCH.lmp` runs ~60 s.

## Doom: renderer source-level costs

Found in the hot-path code review (2026-09-13).

- **Span and low-detail drawers reload their globals per pixel.**
  `R_DrawSpan`, `R_DrawSpanLow` and `R_DrawColumnLow` read their
  colormap and source pointers through globals the byte store may
  alias, so both are reloaded every pixel — 8 cycles on penumbra1, 2
  on penumbra2_5.  `b82fb4b0df29` hoisted them into locals in
  `R_DrawColumn` only.  With the countdown-exit fix as well, the span
  loop goes from 23 instructions to 15.
- **`RANGECHECK` is defined** (`doomdef.h`), adding 4–5
  compare-and-branch pairs to every drawer call.
- **Hot helpers are out of line in other files.**  `FixedMul`
  (`m_fixed_penumbra.c`) is 5 instructions but always called: about 9
  cycles of call overhead on penumbra2_5, as much as its `MUL`.
  `SlopeDiv` and `R_PointToAngle` likewise.  `static inline`
  definitions in a header remove it.
- **BSP leaves recurse to learn that they are leaves.**
  `R_RenderBSPNode` is entered for a subsector child only to test
  `NF_SUBSECTOR` and call `R_Subsector`, paying the full frame.
  Testing the flag in the parent before recursing removes a frame per
  leaf, whatever the shrink-wrapping and tail-call work delivers.
- **Every wall column piece pays a three-call lump lookup.**
  `R_GetColumn` → `W_CacheLumpNum` → `Z_ChangeTag2` runs ~100
  instructions with three call/return pairs and 12 stores, 11 of them
  register saves: `W_CacheLumpNum` saves six registers on its hit path
  for the sake of its miss path (`Z_Malloc`, `W_Read`).  A mapped lump
  skips `Z_ChangeTag2`, and its address never changes, so a column
  pointer table built alongside each texture's lookup would make
  `R_GetColumn` two or three loads.  Lumps read into the zone
  (misaligned ones) can be purged and still need the lookup.
- **Each wall column divides and calls.**  `R_RenderSegLoop` computes
  `dc_iscale` with a `DIV` (34 cycles on penumbra2_5) and the texture
  column with a `FixedMul` call, per column; masked columns divide
  too.

## Doom: R_Init reads every texture patch and sprite

The long silent pause after R_Init's first 16 dots is
`R_GenerateLookup` running over all 963 Freedoom textures.  It
`W_CacheLumpNum`s every patch they use — 1,045 lumps, 10.1 MB — for
each patch's width and `columnofs` table, ~350 KB in all.  The 14 slow
dots after it are `R_InitSpriteLumps` reading all 853 sprite lumps
(2.5 MB) for three header fields each.  At SD speed ("Hardware: SD
read throughput is bottlenecked by pmci_burst byte-PIO") that is over
a minute; id's far smaller IWADs made vanilla's same eager scan cheap.

Reading only headers, or mapping the WAD as the port now does, leaves
the SD traffic as it is: the root filesystem's blocks are 64 KB and
patches average ~10 KB, so every block is read anyway.  Mapping did
remove the copies' memory pressure — faults waiting for memory went
from 53 to none and pagedaemon wakeups from 124 to none.

E1M1 uses 114 textures, 163 patches, 1.1 MB.  Ways to read only those
without stalling play:

- Build a level's lookups during level load, where `R_PrecacheLevel`
  already reads its patches.  Switch and animation frames missing from
  the sidedefs would still load on first sight.
- Cache the computed lookups and sprite dimensions in a file keyed to
  the WAD, so only the first start pays.

Deferring to first use is out: play would stall whenever something
new came into view.

The mapped default reads misaligned lumps into the zone instead
(`LumpIsMapped` in `w_wad.c`); that path has not yet run with an
unpadded WAD such as id's.

## Doom: FastDoom techniques that transfer

Studied 2026-09-18 from FastDoom (github.com/viti95/FastDoom), a DOS
fork tuned for 386 and 486; paths are under its `FASTDOOM/`, and much
of its asm is id's original.  x86-only tricks do not transfer: `lea`
chains, immediates patched by self-modifying code, `shld` span
extraction, Pentium and MMX variants, planar VGA.  Those that cut work
per pixel or per store do, in rough order of payoff here:

1. **Low and potato detail with wide stores** (`linearl.asm`,
   `linearp.asm`): each texel written once as an `STH` or `STW`.
   Halves or quarters the pixels, the stores and the per-column setup.
   Chocolate Doom has low detail; potato would be new.
2. **Column drawer.**  A fraction kept pre-shifted to 7.25 wraps with
   `>> 25` and no `& 127` — the disabled unrolled `R_DrawColumn` in
   `r_draw.c` already does this.  Unroll ×8 with constant-offset
   `STB [dest + k*320]` and enter through a computed jump; FastDoom's
   full 200-way unroll (`linearh.asm`) is 6.4 KB, past the I-cache.
3. **Spans storing four pixels per `STW`** — `planar2.asm` pairs two
   per 16-bit store.  One write-through store in place of four.
4. **Direct drawers** for columns drawn 1:1 — sky, player weapon
   (`linearh5.asm`) — with no fraction arithmetic.
5. **Flat-shaded visplanes** (`r_plane.c` `R_DrawPlanesFlatter*`,
   colour from `R_GetFlatColor`): FastDoom's largest measured gain,
   but a visual trade-off, so an option at most.
6. **Status bar copied only when it changes**: vanilla's
   `updatestate`, which Chocolate Doom dropped.

Its iterative BSP walk (after GBADoom) buys little here, since the
recursion is ~0.1% of a frame ("Doom: where a frame goes").  Drawing
straight into the uncached framebuffer would save the 64 KB copy each
frame — under write-through every store reaches the bus anyway — but
tears without page flipping and makes fuzz read uncached memory;
unmeasured.

## Compiler: IRTranslator runs without branch probabilities — RESOLVED

`PenumbraPassConfig::addIRTranslator` built the translator at its
default `CodeGenOptLevel::None`, so it never requested
`BranchProbabilityInfo`.  Every conditional edge was recorded 50/50,
and block placement ignored `__predict_false`, `__builtin_expect`,
cold calls and loop-exit heuristics; the same probabilities order
switch clusters.  It now passes `getOptLevel()`, as RISC-V and AArch64
do.  Shape coverage: `branch-probability-layout.ll`.

Measured statically on the same IR (kernel `GENERIC.DEBUG`, Doom,
PIC `libsqlite3`):

| | kernel | Doom | SQLite |
|---|---:|---:|---:|
| `.text` | +1.5% | +0.19% | −0.77% |

| `kern_assert` call reached by | before | after |
|---|---:|---:|
| fall-through | 6,730 | 732 |
| taken branch | 139 | 6,133 |

`kern_assert` returns, so each block moved out of line pays a `b` back
— most of the kernel's growth.  Hot functions stay within a few
instructions of their old size.  On hardware the pbench kernel rows
all improved except `clock_gettime`, which retired slightly more
instructions per call although nothing on its path grew — see "pack
the hottest syscall-path text onto shared pages".

Turning it on exposed a latent PIC bug: `lowerAnchoredOperand` read an
offset from jump-table-index operands, which have none, and emitted
whatever the operand storage held.  It had always held zero before —
all 1,895 PIC jump-table bases in the installed shared libraries check
out.

## Compiler: countdown loop exits pay for an overflow flag — RESOLVED

`do { ... } while (count--)` loops — all eight Doom column and span
drawers — ended in eight instructions where two do:

```
lli r6,1; add r1,r6; mov r6,lr; adc r6,zero; lli r7,1; xor r6,r7; and r6,1; bne loop
add r1,1; bne loop
```

`lr` held a zero for the whole loop, and `r7` was an extra
callee-saved register spent on the sequence.

LSR turns the down-counter into an up-counter whose exit test is
`iv + 1 == 0`, and CodeGenPrepare rewrote that test as
`uadd.with.overflow`.  Selection has no branch on an overflow op's
carry, so it materialized the carry, inverted it through the
branch-inversion `G_XOR`, masked it and tested it.

`PenumbraISelLowering` now refuses the rewrite in
`shouldFormOverflowOp` for register-width types.  The compare selects
next to its branch, and compare elimination reuses the add's flags.
Wider adds keep the overflow form: it narrows onto the `ADD`/`ADC`
chain, whose last carry is the wrap, while the compare would expand
as in "64-bit relational compares expand to select diamonds".
Refusing both widths cost `bintime_addx` — inlined into `binuptime`,
`nanouptime` and `microuptime` on every `clock_gettime` — 7 to 10
instructions and extra branches.  Branching on the carry
from inside the selector was rejected: the instructions between the
add and the branch are still generic there, so whether SR survives
would have to be guessed from how they will select.  Selection
places flag producers beside their consumers; flag reuse belongs to
the peephole pass, which sees each instruction's real SR defs.
Shape coverage: `wrap-check-branch.ll`.

`R_DrawColumn`'s loop is 12 instructions per pixel instead of 18, and
the function saves 3 registers instead of 4.  Together with the
inverted-boolean fix in "compare and branch selection misses
immediate and TEST forms", measured statically on the hot-path review
corpus (2026-09-13):

| | `.text` | callee-saved saves | carry reads (`adc x, zero`) |
|---|---:|---:|---:|
| Doom | −844 B | 4,664 → 4,656 | 145 → 134 |
| kernel `GENERIC.DEBUG` | −10,144 B | 35,947 → 35,937 | 1,199 → 1,146 |
| SQLite (PIC) | −1,544 B | 14,077 → 14,075 | 806 → 794 |

A general wrap check, `a + b < a`, selects as `add; cmp sum, a; bcc`,
and compare elimination then drops the compare: the sum is below an
addend exactly when the add wrapped, so the add's C is the compare's
inverted, and readers of C alone swap `BCC`/`BCS`.  On the corpus it
fires only in the kernel, in 4 functions.  Shape coverage:
`compare-elim.ll`.

Open: a wrap check used as a value still selects as
`cmp; mov; sbc zero; and 1` after the add, where `mov; adc zero` does.
One site in Doom.

## Compiler: constants are not rematerializable

`LLI` and `LLIS` (`PenumbraInstrInfo.td`) are not marked
`isReMaterializable`, and a zero is selected as a generic `COPY $r0`,
which the allocator cannot rematerialize either.  A constant live
across a call or a loop gets a callee-saved register or a stack slot,
never a recompute.

- **Loop constants stay in loops.**  The Localizer sinks a constant to
  its use block, and EarlyMachineLICM hoists a cheap instruction only
  if it is rematerializable, so the constant is rebuilt every
  iteration; riscv32 hoists the same constants.  220 kernel loops,
  including `pmci_burst`'s idle fill: 2 of its 14 cycles per byte on
  penumbra1 (about 3% of an SD block; see "SD read throughput is
  bottlenecked by pmci_burst byte-PIO").
- **Constants in stack slots.**  `sqlite3VdbeExec` keeps `rc = 0` in a
  slot reloaded 218 times and `1` in one reloaded 61 times.
  `libsqlite3` has at least 1,250 constant reloads (939 of them zero)
  and 609 spill stores.  The Localizer adds to it: a constant feeding
  more than two phi edges is not localized (`PhiThreshold = 2`,
  upstream "FIXME: Tune this more"), so the VDBE's zero becomes one
  function-wide value.
- **Constants and cheap derived values in callee-saved registers.**
  In Doom's `R_RenderBSPNode`, `side ^ 1` and `bsp + 48` each hold a
  callee-saved register; `solidsegs + 4` and `+ 256` do the same in
  `R_CheckBBox`, `R_AddLine` and `R_Subsector`, because an `ADDi` on
  the address pseudo is not trivially rematerializable.  The address
  case belongs to layer 3 of "G_GLOBAL_VALUE materialization defeats
  CSE and rematerialization".

Fix:
1. Mark `LLI`/`LLIS` rematerializable.
2. Select zero into a form the allocator can rematerialize — a
   `MOV rd, R0` that `isReMaterializableImpl` accepts, the way RISC-V
   accepts `ADDI rd, x0, imm`.
3. Full 32-bit constants: `LUI` reads its destination, so `LLI; LUI`
   is a two-instruction chain that remat cannot copy.  It needs a
   single-def `PseudoLI` expanded after register allocation, like
   `PseudoMOVADDR`.

How aggressively the allocator uses remat is per-core policy — see
"Remat aggressiveness is per-microarchitecture, not a pseudo flag".

## Compiler: constant-offset global addresses stay in the entry block — RESOLVED

For `&global + C` (and `&local + C`) the IRTranslator emits the address
and a constant `G_PTR_ADD` in the entry block.  The Localizer moves an
instruction to its use blocks only if `shouldLocalize` accepts it, and
the default refused `G_PTR_ADD`, pinning the address beside it: built
on every call, held in a callee-saved register or a stack slot, and in
the way of shrink-wrapping.  `PenumbraISelLowering::shouldLocalize`
now accepts a pointer add whose operands the Localizer would itself
move — globals, frame indices, constants — and leaves an add of a real
pointer alone, where cloning would only keep that pointer live longer.
AArch64 accepts every `G_PTR_ADD`; on the same corpus that bought 0.4%
fewer kernel stack-slot operations for more text, so the narrower rule
stays.  Shape coverage: `localize-ptr-add.ll`.

Entry-block sites before the change (post-Localizer MIR): kernel 1,292
in 514 functions, Doom 1,037 in 188, SQLite 2,787 in 655.  Measured
statically on the same IR:

| | `.text` | `[sp]` memory ops | callee-saved saves |
|---|---:|---:|---:|
| kernel `GENERIC.DEBUG` | −0.19% | −1.2% | −1.1% |
| Doom | −0.63% | −9.3% | −5.3% |
| SQLite (PIC) | +1.37% | −8.1% | −5.9% |

SQLite's growth is PIC address builds (`lli; lui; add pc`) duplicated
into the cold blocks that use them — setup, teardown and FTS code; the
hot b-tree and VDBE functions move by tens of bytes.  `btreeNext` now
saves 5 registers instead of 7 and reaches its first load 12
instructions sooner.

## Compiler: early MachineLICM hoists global addresses into callee-saved registers

Early MachineLICM hoists global address builds out of a loop containing
calls, and the allocator then keeps them in callee-saved registers
instead of recomputing them after each call.  Doom's `R_RenderBSPNode`
hoists `@viewx`/`@viewy` out of its far-child loop (3 calls).  With
`-disable-machine-licm` the frame shrinks from 28 to 20 bytes and the
saves from 7 to 5 — 24 cycles per call on penumbra1, 12 on
penumbra2_5.  riscv32 does the same, but its stores are not
write-through.  The allocator takes the trade because a callee-saved
register's first use costs it nothing (see item 1 of the
shrink-wrapping entry's open list).  Localizing constant-offset addresses does not
change this function.

## Compiler: pointer-minus-constant and integer MMIO addresses miss their folds

- **`p - K`.**  `G_PTR_ADD p, -K` selects as `LLIS t,-K; ADD p,t`, often
  with a `MOV`.  `ADDi`/`SUBi` take a zero-extended immediate, and
  `penumbra_neg_imm_to_opposite` rewrites only `G_ADD`/`G_SUB`.  322
  kernel sites (2026-09-13).  The hot ones are the backward loops of
  the C `memmove` from `lib/kern`.  A reduced copy of its byte loop is
  10 instructions per iteration where 6 do:

  ```
  mov r4,r1; mov r11,r2; ldb r2,[r11+0]; llis r1,-1; add r1,r4
  stb r2,[r4+-1]; llis r2,-1; add r2,r11; sub r3,1; bne
  ```

  The fix is the pointer analogue of the existing rule: select a
  `G_PTR_ADD` by a negative constant whose negation fits uimm16 as
  `SUBi`, or rewrite it through the integer subtract that
  `penumbra_ptr_add_neg_to_sub` already builds.
- **MMIO addresses built as integers.**  `bus_space_read_*` and
  `bus_space_write_*` (`bus_funcs.h`) add the handle and offset as
  integers and cast the sum, so the load sees
  `G_INTTOPTR(G_ADD h, C)`.  `selectAddrRegImm` looks only through
  `G_PTR_ADD` and frame indices, giving `ADDi; LDW [r+0]` — plus a
  `MOV` when `h` stays live — where `LDW [h+C]` does it.  187 kernel
  sites, 76 of them with the `MOV`, mostly in `pmci`, `pdisplay`,
  `pusbhc` and `paudio`.  Either look through
  `G_INTTOPTR(G_ADD x, C)` at selection, or do the macros' arithmetic
  on a `char *` — `bus_funcs.h` is MD code.  RISC-V's GlobalISel has
  the same selection gap.

## Compiler: InstCombine's shift-and-mask form costs a constant

InstCombine canonicalizes `(x >> C1) << C2` with `C1 > C2` into one
shift and a mask, `(x >> (C1 - C2)) & -(1 << C2)`.  On x86 and AArch64
the mask is a single instruction.  Penumbra's `ANDi` takes a
zero-extended 16-bit immediate, so a mask of high ones becomes
`llis t, -(1 << C2); and t, x`: two instructions and a scratch
register where a second shift is one.

Found in the fractal demos (2026-09-13).  `qmul(zr, zi) * 2` —
`trunc((a * b) >> 28) << 1` — came out as `lshr i64 %p, 27`, `trunc`,
`and i32 %t, -2`: five instructions per iteration after i64 narrowing,
where `shr lo, 28; shl hi, 4; or; add x, x` is four, in a loop with no
free register for the constant.  The demos now write 2ab as one shift
(`qmul2`) and no longer show it.

Fix: a Penumbra combine rewriting `G_AND (G_LSHR y, c), -(1 << k)`,
also through a `G_TRUNC`, into `G_SHL (G_LSHR y, c + k), k` when the
mask does not fit the immediate.

The mask on its own is far more common and a different trade.  A
static scan (2026-09-13) finds `llis -2^k; and` 325 times in the kernel,
128 in `libsqlite3` and 11 in Doom, almost none right after a shift —
round-down alignment masks.  `shr x, k; shl x, k` matches their
instruction count and frees the scratch register, but overwrites `x`,
so it only wins where `x` is dead afterwards.

## Compiler: G_GLOBAL_VALUE materialization defeats CSE and rematerialization

`PenumbraInstructionSelector` lowers `G_GLOBAL_VALUE` (and the
`@sym + offset` form) unconditionally to a raw `LLI rd, %lo ; LUI rd,
%hi` pair at selection time, in `emitLoadSymbolAddr` (the static/absolute
path; PIC takes the GOT path below). That eager two-instruction
expansion is a *double* defeat:

- **CSE can't see a shared base.** Each pair materializes the full
  `sym+offset`, so two references to the same symbol at different
  offsets — or two globals the linker happens to place near each other —
  look like unrelated constants. MachineCSE has nothing to share.
- **Rematerialization can't fire.** An address is the textbook remat
  candidate (cheap to recompute, no inputs), but remat works on a single
  def, not a two-instruction dependent chain. So when an address must
  survive a call, the allocator's only options are spill/reload or
  burning a callee-saved register — never "recompute it after the call,"
  which is usually cheapest.

A representative kernel sequence (hot path) materializes four addresses
that are all `copyright + constant` through four independent pairs — two
of them 11 bytes apart:

```
lli r1, 0xF1DD ; lui r1, 0x8027   ; copyright+0x13dc3
lli r2, 0x6D84 ; lui r2, 0x8028   ; copyright+0x1b96a
lli r3, 0x0A7E ; lui r3, 0x8027   ; copyright+0x5664
lli r4, 0x0A89 ; lui r4, 0x8027   ; copyright+0x566f  (11 B past r3)
```

Dynamic share on Dhrystone (new ISS `+opstats` instruction-class
histogram): explicit `LLI/LUI` is ~5% of executed instructions, but the
absence of base-sharing also inflates the immediate-ALU bucket (~30% —
offset arithmetic that would otherwise fold into a load) and, in
disassembly, the pattern is pervasive on hot paths.

### Instruction-count vs footprint: which layer moves which

The "Dhrystone hot-path code shape" entry above established that
I-footprint, not dynamic instruction count, gates HW Dhrystone —
count-*neutral* folds (the select-CMPi fold) measured flat because they
don't change which cache lines are touched. Be precise about which layer
of this fix is count-neutral and which actually removes instructions:

- **Layer 1 is count-neutral by construction.** `PseudoMOVADDR` expands
  back to the identical `LLI+LUI`, so the static instruction stream is
  unchanged unless CSE or remat fires. MachineCSE already deduped eager
  `LLI+LUI` pairs in straight-line code, so layer 1 adds little there;
  its real win is *rematerialization* — trading a spill/reload (or a
  burned callee-saved register) for a recompute when an address is live
  across a call under register pressure. That is a register-allocation
  *quality* win, not a footprint win, and it only engages where those
  cross-call-live address sites exist. It is the concrete mechanism
  behind that entry's finding #1 ("register-pressure-aware
  address-arithmetic CSE").
- **Layer 2 is infeasible on Penumbra; layer 3 still removes
  instructions (corrected 2026-06-22).** The textbook RISC-V fold —
  materialize a `%hi` base and let the load carry `%lo` in its offset
  field (`LUI + LD %lo`, 2 insns) — does **not** apply: Penumbra's `LUI`
  *ORs* the high half in (`Rd = Rd | (imm16<<16)`,
  `instruction-set.md`; ISS `case 2`), it does **not** clear the low 16
  bits, so a `%hi`-only base would first need `MOV rd,R0` — `MOV+LUI+LD`
  is three instructions, identical to `LLI+LUI+LD`. A single isolated
  global load is therefore structurally **3 instructions** with no
  compiler fold to 2 (closing that would need a clear-low load-upper
  instruction — an ISA change, weighed against the discrete-logic
  constraint; the OR-`LUI` exists to make the `LLI`-then-`LUI` idiom
  work without a separate combine). What *does* remove instructions is
  **base-sharing**: with the layer-1 pseudo, MachineCSE materializes a
  reused base once and each access folds its *residual constant offset*
  into the load's simm16 field when it fits ±32 KB (the layer-3
  mechanism), collapsing redundant `LLI+LUI` pairs and offset `ADD`s.
  That helps clustered/reused globals, not the isolated single-access
  case — which is most of Dhrystone's near-use globals, consistent with
  the measured-flat result below.

**Measured 2026-06-18 (HW): layer 1 alone is flat on Dhrystone** — as
expected from the above. Dhrystone accesses its globals near their use,
not held across calls, so remat has nothing to engage and CSE already
captured the straight-line case. Layer 1's value is the *enabler*
(single SSA def that layers 2-3 and the allocator build on) plus
RA-quality on cross-call, register-pressure-bound code — kernel-shaped,
not Dhrystone-shaped. To see layer 1 move a number, measure a workload
with cross-call address liveness (kernel syscall / fork+exec benches).
Footprint only shrinks where a base is *reused* (base-sharing collapses
the redundant `LLI+LUI`) or carries an offset that fits the load's
simm16; Dhrystone's isolated near-use globals largely lack that shape,
so even the full fix may stay flat there. The per-load `%lo`-into-load
shrink once hoped for here is infeasible (OR-`LUI`, above).

### Fix — three layers (layer 1 enables; base-sharing + layer-3 offset-merge carry the footprint win; the RISC-V `%lo`-into-load fold is infeasible)

1. **Rematerializable materialization pseudo.** Stop emitting `LLI+LUI`
   at selection; lower the static/absolute `G_GLOBAL_VALUE` to a single
   `PseudoMOVADDR rd, @sym` (`isReMaterializable`, `isPseudo`,
   `Size = 8`), expanded to `LLI+LUI` in a *post-RA*
   `PenumbraExpandPseudoInsts`. Now it is one SSA def: MachineCSE shares
   identical bases pre-RA, and the allocator owns the cross-call
   keep-vs-recompute decision via remat — exactly how AArch64 never
   "carries" a base over a call (its `ADRP` is a rematerializable single
   instruction it just recomputes after the call). This one change fixes
   the kernel example and the cross-call carrying; the rest is gravy.
   Expand it in a *post-RA* pass (a new `PenumbraExpandPseudoInsts`, or
   the `PenumbraInstrInfo::expandPostRAPseudo` hook) — **not** at
   AsmPrinter time: the pseudo must survive register allocation as one
   `MachineInstr` for the allocator's `reMaterialize` to copy it. Note
   the existing PIC carriers (`PICLLI`/`PICLUI`/`PICADDPC`) expand inside
   `PenumbraAsmPrinter` because their operands are label-difference,
   MC-level expressions — that is the right idiom for *PIC*, the wrong
   one for this expansion site. RISC-V's `PseudoLLA` + post-RA
   `RISCVExpandPseudoInsts` is the matching template.
2. **~~Fold `%lo` into the load offset~~ — infeasible on Penumbra
   (corrected 2026-06-22).** The RISC-V form (`LUI + LD %lo`) assumes a
   clear-low `LUI`; Penumbra's `LUI` ORs the high half without clearing
   the low 16 (`Rd = Rd | (imm16<<16)`, `instruction-set.md`; ISS
   `case 2`), so a `%hi`-only base cannot be built in one instruction and
   the fold saves nothing — there is no symbol-`%lo`-into-load
   instruction-count win (see "which layer moves which" above). The
   surviving instruction-removal is the **constant-offset** fold under
   layer 3, *not* the symbol-`%lo` fold: after base-sharing materializes
   a base once, fold a residual constant offset that fits the load's
   simm16 field directly into the memory op (`LDW [base + off]`), else
   keep the `ADD`. The `isLegalAddressingMode` base+simm16 advertisement
   is still worth fixing for that — the same gap as the "[R0 + offset]
   absolute addressing" entry, which remains independently valid (R0 is
   hardwired zero, so a small absolute address rides the load's offset
   with no `%hi` at all). `selectAddrRegImm` already folds a *resolved*
   base+simm16 into the Format-M offset field for frame indices and
   constant GEPs; (a) confirm the Format-M memory-offset operand can
   carry a *symbolic* relocation for the offset-merge case, and (b) drop
   the `HasBaseReg`/base+simm16 reject in `isLegalAddressingMode`.
3. **Merge residual constant offsets** into the pseudo's symbol operand
   (`%hi(sym+o)`) and into memory ops, modeled on RISC-V's
   `RISCVMergeBaseOffset` — for offsets that can't fold into a load
   (out of range, or an address materialized into a register, e.g. a
   pointer argument as in the kernel example).

Note the cross-call case is **not** a job for a greedy merge pass — it
is a register-allocation cost decision (remat vs CSR vs spill), and the
pseudo is precisely what hands it to the allocator. Don't hand-roll
cross-call liveness in a MIR pass.

### Remat aggressiveness is per-microarchitecture, not a pseudo flag

Split *capability* from *policy*. The pseudo's capability attributes
(`isReMaterializable`, `Size = 8`, `hasSideEffects = 0`) are
generation-neutral and stay unconditional in TableGen — every Penumbra
core can recompute an absolute address. How *aggressively* the allocator
should exploit that is microarch-specific: remat duplicates the 8-byte
sequence, which is cheap to hide in gen2's 4 KB 2-way L1 but can evict a
hot line in gen1's 1 KB direct-mapped I$. That tuning is policy, so it
belongs in the per-subtarget scheduling/cost model (today a single
`penumbra1` `ProcessorModel`; `-mcpu` already selects it, default
`penumbra1`), gated behind a `SubtargetFeature` when a `penumbra2` model
lands — never baked into the pseudo's static flags. For the first cut:
enable remat, leave `isAsCheapAsAMove` *off* (it tells the sinker the
sequence is free and invites per-use-site duplication), and tune
conservatively for gen1; flip the policy via a feature bit when the
genN subtargets split. The same per-subtarget seam will host any later
cost-model divergence (branch cost, alignment padding, LSR weights), so
when `penumbra2` is added, audit these together rather than one flag at
a time.

### TLS and PIC are orthogonal

Only the static/absolute path changes. PIC (`G_GLOBAL_VALUE` → GOT load)
is a *load*, not a materialization — CSE-able but **not** rematerializable
(it touches memory), so it gets its own pseudo with different properties.
TLS (GD/LD/IE/LE) is model-specific (thread pointer, possibly a
`__tls_get_addr` call) and stays its own path. The pseudo model actually
*simplifies* the current selector tangle: emit a distinct pseudo per
relocation/TLS model and move the byte sequences into the post-RA
expander, the way RISC-V's `PseudoLA` / `PseudoLA_TLS_*` family does.

### Templates and tooling

- RISC-V: `PseudoLLA`/`PseudoLA`/`PseudoLA_TLS_*` in `RISCVInstrInfo.td`,
  the post-RA `RISCVExpandPseudoInsts.cpp`, and `RISCVMergeBaseOffset.cpp`
  are near-direct templates (Penumbra's static expansion is absolute
  `LLI+LUI` where RISC-V's is PC-relative `auipc+addi`).
- ISS diagnosis tooling added this round: `+opstats` (instruction-class
  histogram), `+branchstats` (branch-direction histogram), and
  `sw/tools/branch_ceiling.py`. Re-run Dhrystone with `+opstats` after
  layer 1 to confirm the immediate-ALU / `LLI`+`LUI` buckets drop.

### 2026-06-19 empirical re-measurement — Layer 2 doesn't translate; base-sharing is the static lever

A disassembly study of the just-built binaries (kernel `MINIMAL`, an
`ET_EXEC` executable, libc.so) reshaped the plan above. Method:
`llvm-objdump -d` annotates every `lui` with its resolved 32-bit value
(the disassembler tracks GPR state), so address-forming `lli+lui` pairs
can be bucketed by target. Filtering the kernel to values that land
inside the kernel image (`0x80010000`–`0x802c0000`) drops 32-bit
*constant* materialisations and leaves **41,590 true absolute address
materialisations**. Findings:

- **Same-symbol multi-offset is already optimal.** Struct-field and
  array-element accesses (`s.a/s.b/s.d`, `g[2]/g[5]`) already materialise
  the base once and fold each offset into the load
  (`selectAddrRegImm`): `2 + N`, not `3N`. Layer 1's CSE-able pseudo plus
  the existing constant-offset fold delivers the base-amortisation win
  Layer 2 was reaching for — and the base is the *exact* `&s`, so the
  hardware's signed-offset add is correct with no carry handling.
- **Layer 2's per-access `LLI+LUI+LD → LUI+LD` win cannot exist here.**
  That reduction assumes a one-instruction high-only base. RISC-V's `lui`
  *replaces* the low bits; Penumbra's `LUI` **ORs** (`Rd = Rd |
  (imm16<<16)`, encoding-table opcode `0010`, tied `$Rd=$Rd_in`), so a
  clean `hi<<16` base needs `LLI rd,#0; LUI rd,%hi` — two instructions —
  and folding `%lo` into the load buys nothing. Layer 2 ports the RISC-V
  *mechanism* but not its *benefit*. **Treat Layer 2 (the `%lo`-into-
  offset fold) as not applicable; do not implement it as written.**
- **A "load-upper-and-clear" (`LUIC`) opcode is a minor lever, not the
  fix.** It would enable `LUIC %hi_carry; LD %lo` (3→2), but only **11.2%**
  of kernel address materialisations feed a single direct `[Rd+0]`
  load/store where the fold applies (~4,654 sites, ~18 KB `.text`); the
  other ~62% use the address as a pointer/base, where `LUIC %hi; ADDi
  %lo` = 2 = the existing `LLI;LUI`, no gain. It also does **not** merge
  unrelated-but-close symbols (different symbols → different `%hi_carry`
  relocations that don't CSE at compile time). Encoding space exists
  (Format-L opcodes `1101/1110/1111` reserved); RTL is trivial and
  actually drops the `Rd` read/tied operand vs OR-`LUI`. But it must be
  *additive* — the `LLI;LUI` 32-bit-constant idiom needs OR semantics, so
  `LUIC` cannot replace `LUI` — and it needs a carry-adjusted `%hi`
  relocation. Verdict: defer; sequence after base-sharing (which already
  converts many `[Rd+0]` loads to `[base+off]`, overlapping its win) and
  re-measure.
- **Base-sharing of unrelated-but-close absolute addresses is the real
  static lever** — the `copyright+const` shape, e.g. `phys_bias`
  (`0x80297000`), `bootinfo_store` (`0x80297020`), `penumbra_bootinfo`
  (`0x8029802c`) each rebuilt from scratch in the same `lui 32809` page.
  Of the 41,590 materialisations, **40.9% are shareable under a
  conservative model** (cluster targets within 60 KB inside a straight-
  line run split on calls), 78.1% function-wide. This is an *upper
  bound*: holding a base live across a call-free region still competes
  for one of ~12 allocatable registers in a 2-operand ISA, so the
  realisable share is lower; the clean first cut is call-free,
  same-section clusters.
- **The transform is layout-neutral, no ISA change needed.** For two
  *local same-section* symbols `A`,`B`, the distance `B-A` is an
  assembly-time label difference (the assembler folds it — no
  relocation, no carry, and the data never moves). So a cluster of K
  references becomes `LLI %lo(A); LUI %hi(A)` (exact `&A`) + K loads at
  `[&A + (sym_i - A)]` = `2 + K` vs `3K`. This is GlobalMerge's win
  *without* GlobalMerge's data-relocation cache penalty — GlobalMerge
  physically coalesces globals into one struct, which measured −2.4%
  DMIPS on gen1 Dhrystone (fewer instructions but more misses in the
  1 KB direct-mapped L1 from the relocated data). Label-difference
  base-sharing moves no data, so that penalty never arises, on either
  generation. (Whether the original GlobalMerge penalty still holds on
  gen2's 4 KB 2-way L1 is untested and moot for this approach.)
  Cross-section clusters need a link-time relocation but stay
  layout-neutral.
- **Implementation shape:** a target MIR pass over the `PseudoMOVADDR`
  defs (Layer 1 already gives CSE-able single-def bases), modelled on
  `RISCVMergeBaseOffset` but extended from same-symbol to same-section
  cross-symbol. Open scope decision: same-section-local only
  (assembly-time diffs, zero new relocations — recommended first cut) vs
  also cross-section (needs a base-relative `R_PENUMBRA_*` and the
  linker). The register-pressure call belongs in the allocator, not a
  greedy merge — same caution as the cross-call remat note above.

`fold_classify`/clustering scripts used for these numbers are throwaway
disassembly miners (not committed); the durable artifacts are the
numbers and conclusions here.

**In-loop address pairs in the demos are remat, not a missed hoist.**
Plasma's depth-3 pixel loop shows an `LLI`/`LUI` pair per iteration
re-materializing a table base.  Post-regalloc MIR holds a single
`PseudoMOVADDR`; the pair appears only after `expandPostRAPseudo`, so
Layer 1 is doing its job and LICM is not the lever.

What drives the remat is the destructive two-operand `ADD` forming
`base + index`: it consumes the base, so each address computation needs
either a private copy of a live base or a fresh materialization.  The
same loop shows both choices — `COPY` + `ADD` for a use with the base
still live, a bare `ADD` killing it at the last use, then
`PseudoMOVADDR` at the next block that needs it.  Rebuilding costs two
instructions where copying a live base costs one, so remat runs one
instruction per materialization *more* than pinning the base; the
allocator takes that trade because the loop is at near-total pressure
(~10 of 11 allocatable GPRs live across it) and a spill/reload would
cost more.

Nothing cheaper is available: an arbitrary global's address is
inherently a two-instruction materialization, so the +1 is structural
and the only alternative is spending a register to keep the base live.
That makes this a register-pressure trade for the allocator, not a
missing optimization.  Recorded so the shape isn't re-diagnosed as a
missed hoist.

## Compiler: PIC/GOT global access — non-preemptible direct addressing — RESOLVED

The high-leverage lever — PC-relative-direct addressing for
non-preemptible PIC globals — landed in `8c8c28df7f51`.
`PenumbraInstructionSelector::selectGlobalValue` now gates on
`!GV->hasExternalWeakLinkage() && GV->isDSOLocal()`: a non-preemptible
symbol materialises its address with `%pcrel` + a single deref, skipping
the GOT slot, the GOT load (which sat on the critical path — the deref
could not issue until it returned), and the startup `R_PENUMBRA_RELATIVE`
reloc. A non-zero offset folds straight into `%pcrel_lo16(sym+off)`,
which the GOT path could not do (a GOT entry holds the base symbol only).
Undefined `extern_weak` stays GOT-indirect even when hidden — it may
resolve to 0, which a `%pcrel` anchor cannot encode. Shape coverage:
`pic-dso-local.ll`; execution coverage: `pcrel-dso-local.c` in the
`make test-compiler-pic` suite.

**Measured on a fresh NetBSD `libc.so.12.220.1` (reloc histogram +
`.got`-range bucketing via `llvm-readobj --dyn-relocations`), against the
pre-fix 2026-06-19 baseline:**

| Metric | Pre-fix | Now |
|--------|---------|-----|
| GOT slots bound by `RELATIVE` (non-preemptible) | ~5,028 | **56** |
| GOT slots bound by `GLOB_DAT` (preemptible) | 237 | **237** |
| Total `.got` data entries | ~5,265 | **299** |

The non-preemptible GOT population collapsed ~99%. `GLOB_DAT` is
unchanged to the symbol — proof the change is preemptibility-safe: it
removed only the slots it was entitled to. Of the 2,291 `RELATIVE`
relocs left in the binary, only 56 land in `.got`; the rest are ordinary
`.data`/`.data.rel.ro` pointer relocs, never a GOT-codegen concern.

**Why this was the dominant cost (kept for context):** the GOT cost lived
in the `ET_DYN` shared libraries, where each global read was a
5-instruction GOT-indirect sequence (`lli; lui; add rX,pc; ldw(GOT);
ldw(deref)`) and the PC-anchored immediates made every site unique,
defeating CSE. Dynamic relocs ran ~20:1 `RELATIVE`:`GLOB_DAT`, i.e. the
large majority of GOT-bound symbols were non-preemptible and never needed
a slot. Executables (`ET_EXEC`) are a *separate* problem — their own
globals use absolute `lli+lui` (base-sharing applies), not the GOT; calls
into libc go through the PLT (`JUMP_SLOT`).

**Residual GOT slots, not worth pursuing** (the GOT cost itself is now
too small to justify more work):

- The **56** non-preemptible GOT slots that remain are symbols the
  compiler could not prove `dso_local` at compile time but the linker
  bound locally (default-visibility data globals, function-address
  takes). Below the bar for a dedicated pass.
- A **GOT/GP base register** for the preemptible remainder would target
  only ~293 slots, and the 2-operand ISA makes a pinned GP costly in
  register pressure.

**The follow-on lever is NOT closed by this fix — it grew.** Each of the
~5,000 now-direct accesses still carries its *own* per-site
`lli; lui; add pc` triple anchored to its own PC, so repeats do not CSE.
**PC-anchor sharing** (regional base-CSE: materialise one PC-derived
anchor per region, then `anchor + (sym - .Lanchor)` per access) collapses
those triples. It is **layout-neutral — it does not relocate data**, so
it is the *opposite* transform from GlobalMerge: the net-negative
Dhrystone result belongs to GlobalMerge (which packed hot scalars beside
a cold 10 KB array and thrashed the **gen1 1 KB direct-mapped** D-cache),
*not* to base-CSE, whose only cost is register pressure. That gen1 cache
number does not transfer to gen2 (4 KB 2-way) / gen3, which is where
userland runs. This is the same pass as absolute base-sharing for the
kernel + executables' own globals, differing only in base kind
(PC-derived vs absolute) — tracked under "G_GLOBAL_VALUE materialization
defeats CSE and rematerialization". Realized win is global-density
dependent (real code is far less global-dense than Dhrystone), so measure
on gen2/gen3 hardware before productionising.

**Priority across the remaining addressing investigation, by leverage:**
(1) regional base-CSE — one parameterised pass serving both PC-anchor
sharing (the ~5,000 direct PIC accesses above) and absolute base-sharing
for the kernel + executables' own globals (~41% of kernel
materialisations); layout-neutral, register-pressure-gated, measured on
gen2/gen3; (2) `LUIC` and other secondary folds that compose with it.

## Compiler: no branch-cost model — branch-avoidance may be over-eager

Penumbra sets none of the branch/select cost knobs
(`convertSelectOfConstantsToMath`, `predictableSelectIsExpensive`, TTI
`getCFInstrCost`, …), and GlobalISel would not ask the one that matters.
Upstream's `match_selects` (in `select_combines`, part of
`all_combines`) rewrites `select(cmp, C1, C2)` into arithmetic on the
compare result — `tryFoldSelectOfConstants` covers `C` against `C ± 1`,
a power of two or all-ones against zero, and all-ones against anything
— with no cost check.  SelectionDAG gates the same fold on
`TLI.convertSelectOfConstantsToMath(VT)`, which defaults to false and
which RISC-V, X86 and LoongArch opt into; GlobalISel never consults it.
That is a GlobalISel-vs-SelectionDAG gap worth a `doc/llvm-*.md`
write-up.  Penumbra's own branchless icmp-to-value path through the
carry flag is a separate, clear win over the old `SELECT_CC` form and is
not in question.

What the fold and the branch diamond produce, identical under every
tuning:

| Source | Emitted |
|---|---|
| `x == 2 ? 3 : 4` | `sub; cmp; mov; adc; shl; sar; add` — 7, branchless |
| `x < 5 ? 8 : 0` | `mov; lli 1; lli 0; cmp; blt; mov` to build the 0/1, then `shl 3` — 7, a branch and the arithmetic |
| `x == 7 ? 100 : 37` (not folded) | `mov; lli; lli; cmp; beq; mov` — 6 |

Beyond the fold's cost-blindness:

- **Signed conditions pay both costs.**  A signed compare cannot come
  out of the carry flag, so selection builds its 0/1 with a branch
  diamond and the fold's shift or sign extension runs on top.  That is
  a loss on every core, whatever the tuning.
- **The branch form is longer than it needs to be.**  The select
  diamond loads both constants before the compare and then copies
  conditionally.  `LLI` and `MOV` leave the flags alone, so
  `cmp; lli C1; beq; lli C2` is 4 instructions.

Against that 4-instruction form the measured costs
([Branch costs](#branch-costs)) favour the branch on both cores for
`x == 2 ? 3 : 4`:

| Core | Branch form | Branchless (7 instructions) |
|---|---|---|
| penumbra1 | 6–8 cycles, deterministic | 14 cycles |
| penumbra2_5 | 4 cycles, +4 when mispredicted | 7 cycles |

On penumbra2_5 the branch loses only above a 75% misprediction rate, and
the BTB predicts each site's last outcome, so even random data sits near
50%.  The branch form is also the shorter code, so the I-cache effects
the cycle figures leave out point the same way, and the decision does
not wait on a hardware A/B.

In order:

1. Make `tryFoldSelectOfConstants` consult
   `convertSelectOfConstantsToMath`, and write the upstream doc.
   Penumbra's override weighs the tuned core's costs the way
   `isIntDivCheap()` does: penumbra1 decides on instruction counts
   alone, since a `Bcc` costs as much as an ALU operation; penumbra2_5
   adds the misprediction penalty at one named, assumed misprediction
   rate.  That needs branch costs in the scheduling models, which
   [instruction-timing.md](internals/instruction-timing.md) already
   records.  With today's costs the override answers false on both
   cores, which also removes the branch-plus-arithmetic shapes; the cost
   check pays once a core (gen3) moves the numbers.
2. Select a select of two constants as `cmp; lli; bcc; lli`, so the
   check compares against the branch form actually emitted.
3. Separately, InstCombine already delivers `sext(icmp)` for
   `x > y ? -1 : 0`, outside the fold's reach; selection builds a 0/1
   diamond and then `shl 31; sar 31`, where loading -1 directly saves
   two instructions.

Dhrystone's `Proc_6`, `*ref = (x==2) ? Ident_3 : Ident_4`, is the
`C`-against-`C + 1` shape.  Reach check 2026-06-09: that chain is the
*only* such site in Dhrystone's hot path (see the Dhrystone hot-path
entry above), so Dhrystone cannot show the difference either way;
confirm on kernel hot paths.

## Compiler: inline small constant-size memcpy/memset/memmove

`G_MEMCPY`/`G_MEMSET`/`G_MEMMOVE` are `.libcall()`'d unconditionally in
`PenumbraLegalizerInfo.cpp`, so even `memcpy(d, s, 16)` and
`memset(d, 0, 12)` emit a `bl memcpy`/`bl memset` rather than a few
inline word stores. Other GISel targets inline the small constant-size
cases; Penumbra is missing the wiring, not the capability.

The expansion itself is generic: `CombinerHelper::tryCombineMemCpyFamily`
(and `tryEmitMemcpyInline` for `llvm.memcpy.inline`) turns a constant-size
op into a load/store sequence bounded by `TargetLowering::MaxStoresPerMem*`
(base defaults 8, or 4 at `-Os`), shaped by `findOptimalMemOpLowering` ->
`getOptimalMemOpType` + `allowsMisalignedMemoryAccesses`. It is **not** a
`Combine.td` rule, so sitting in `all_combines` does not enable it — each
target hand-wires a dispatch in its pre-legalizer combiner's
`tryCombineAll` override (see `AArch64PreLegalizerCombiner.cpp` and
`MipsPreLegalizerCombiner.cpp`). The legalizer `.libcall()` then remains
the fallback for the variable-size / over-threshold cases. AArch64 also
forces an inline length of 32 bytes at `-O0` and lowers `memset(...,0,...)`
to bzero.

Penumbra currently uses the plain generated `tryCombineAll` (no
`CombineAllMethodName` override, no mem-opcode dispatch) and overrides none
of the mem-op TLI hooks, so `tryCombineMemCpyFamily` is never reached and
the default threshold of 8 sits unused.

To enable it:
- Switch `PenumbraPreLegalizerCombiner` to the `CombineAllMethodName =
  "tryCombineAllImpl"` pattern and hand-write `tryCombineAll` to dispatch
  `G_MEMCPY`/`G_MEMSET`/`G_MEMMOVE`/`G_MEMCPY_INLINE` to the helper before
  falling through to `tryCombineAllImpl`.
- Override `getOptimalMemOpType` to return `s32` — the base default returns
  an invalid `LLT`, so `getMemOps` won't pick word stores otherwise.
- Keep the default `MaxStoresPerMem*` (8 / 4) unless profiling says
  otherwise.

Caveat: Penumbra lowers unaligned loads/stores to byte ops, so an
under-aligned copy would expand into more than the threshold's worth of
byte stores and bounce back to the libcall. The realistic win is therefore
word-aligned constant-size struct/array copies — which is the common case.
Decide via `allowsMisalignedMemoryAccesses` whether to inline unaligned at
all (probably not).

## Compiler: per-generation tuning — models and their consumers

Every Penumbra generation implements the same ISA and accepts the same
binaries, but their micro-architectures pull optimization tradeoffs in
opposite directions.  The backend needs to know which one it is
building for and skew cost-driven choices accordingly, without ever
changing what code is *legal*, so one object file still runs anywhere.

### The invariant that constrains the whole design

Generation knobs are **tuning only**.  They may change *which of
several correct lowerings* the backend picks (branch vs. branchless,
inline vs. libcall, schedule, unroll factor) but must never change
instruction legality or the emitted instruction set.  Legalization
stays identical for every generation; the knobs are consumed only in
cost models, combiner gating, and scheduling.  Concretely: never
branch on a subtarget feature inside `PenumbraLegalizerInfo`'s
legality rules.

This is why the selection is `-mtune`-shaped rather than `-mcpu`-shaped.
A feature belongs in the *tune* list of a `ProcessorModel` (the fourth
argument), never the architecture list: `MCSubtargetInfo` takes
architecture features from the `-mcpu` name and tuning features from
the `-mtune` name, and reads the scheduling model from the tune name
alone.  Putting a Penumbra property in the architecture list would
claim it changes what a binary may contain.

### Infrastructure — landed

`Penumbra.td` defines `penumbra1`, `penumbra2` and `penumbra2_5` as
processors over one shared architecture.  Each carries a
`PenumbraTuning` record pairing its tune features with its scheduling
model: penumbra1 uses `Penumbra1Model`, penumbra2 and penumbra2_5
share `Penumbra2Model`, and all feature lists are empty.  `generic` is
not a tuning of its own: it is defined from the
**most-used variant's** `PenumbraTuning`, so it tunes exactly like
that core rather than averaging across generations, and moving the
default is a one-reference change in `Penumbra.td`.
`PenumbraSubtarget` holds the generated tune-feature fields and
defines what an unspecified name resolves to (`resolveCPU` /
`resolveTuneCPU`), which is `generic`.
`PenumbraTargetMachine::getSubtargetImpl(const Function &)`
builds a subtarget per distinct (cpu, tune, features) combination.

That last piece is a prerequisite, not a refinement: `-mtune` reaches
the backend *only* as a per-function `"tune-cpu"` attribute, so a
single module-wide subtarget could not observe it at all.  `llc` has
no `-mtune` switch, which is why `test/CodeGen/Penumbra/mtune.ll`
drives tuning through IR attributes rather than a command line.

Clang accepts both flags for the Penumbra triple (`getCPUName` in
`CommonArgs.cpp`, `AddPenumbraTargetArgs` for `-mtune`) and diagnoses
an unknown name against the list in `clang/lib/Basic/Targets/
Penumbra.cpp`, which mirrors the `ProcessorModel` definitions.

The models differ only in the MUL and DIV latencies they state, so a
constant divide is the one place the names produce different code so
far.  A core earns a name when builds
target it explicitly or when `generic` follows it; its tuning then
fills in as properties with consumers land.

### The largest divergence: dependence distance

The property that most separates the generations is how far a
consumer must sit from its producer, and it splits them three ways
rather than two:

| Shape | Machines | Scheduling consequence |
|---|---|---|
| No overlap | gen1 (microcoded, single-issue) | Separating a use from its producer buys nothing and costs live range |
| Overlap, full interlock | gen2 (no GPR forwarding, no write-through) | Every producer/consumer pair wants distance |
| Overlap, forwarded | gen2.5 | Only loads want distance, and only one slot |

The gen2 figures are in
[penumbra2/overview.md](internals/penumbra2/overview.md): load-use
stall 4 vs. 1 and CPI on tight cache-hit loops ~2.5-3.0 vs. ~1.2-1.5
(branch costs are under [Branch costs](#branch-costs)).  That CPI gap
is this property.

Expressing it is **not** a feature bit.  Dependence distance is
operand latency plus `ReadAdvance` bypass entries in a
`SchedMachineModel`, which the MachineScheduler consumes; feature bits
gate discrete choices.  A model attaches to a processor, so a
generation whose dependence behaviour differs needs its own processor
entry — that is the argument for splitting a name, and the only one
that requires it.

The models are `CompleteModel = 0`, state `WriteRes` latencies for ALU
operations, MUL and DIV, and carry no `ReadAdvance` entries.  Nothing
schedules against them yet: MachineScheduler runs only for a subtarget
that returns true from `enableMachineScheduler()`, which defaults to
false and Penumbra does not override, so the model's `IssueWidth`,
`MicroOpBufferSize` and `LoadLatency` have never influenced code
generation.  What does read the model today is Penumbra's own cost
code (`PenumbraSubtarget::getALUCycles` and friends, feeding
`getArithmeticInstrCost`, `isIntDivCheap()` and the constant-multiply
combine); upstream, `SelectOptimize` reads `MispredictPenalty` (see
[Branch costs](#branch-costs)).  The other consumers of scheduling data
— MachineCombiner, EarlyIfConversion, MachineLICM's high-latency
hoisting — need target patterns, a target that adds the pass, or a
hook override, and none apply.  Writing a real dependence model
therefore means enabling MachineScheduler as well, annotating every
instruction's reads, and measuring what the pass costs in compile time
and register pressure.  `generic` takes the most-used variant's side
once a second model exists, so a generic build schedules for a
forwarding core; an interlocked or single-issue core that wants its own
schedule is built with its explicit name.

**gen2's dependency cost is not modelled.**  gen2 and gen2.5 share one
scheduling model that states only instruction latencies, so cost
decisions price gen2 as if it forwarded.  On gen2 a consumer waits for
its producer to leave WB: 3 stall cycles at distance 1, fewer further
back ([hazard-model.md](internals/penumbra2/hazard-model.md), Example
A; a chain of eight dependent `ADD`s measures 29 cycles against 8 on
gen2.5).  A shift/add chain is the worst case, since every op reads
the one before it.

LLVM can describe this.  A `ReadAdvance<Read, N, [producers]>` says
when a consumer's operand becomes usable relative to its producer; a
negative `N` is a consumer that must wait (TargetSchedule.td names
this use), and the producer list scopes it — e.g. to loads.
MachineScheduler builds a dependency DAG whose edges carry
`computeOperandLatency` (producer latency minus the consumer's
`ReadAdvance`) and places instructions on a cycle timeline under
`IssueWidth` and in-order issue.  On that timeline a constant `-3` *is*
distance-dependent: a consumer is not ready until three cycles after
its producer, so independent instructions scheduled between them absorb
the stall, which is exactly gen2's behaviour.  Only per-instruction
queries that see one edge at a time — MachineTraceMetrics' critical-path
depth, which also assumes an out-of-order core by taking the longest
path rather than summing — cannot see distance.
`TargetSubtargetInfo::adjustSchedDependency` allows per-edge tweaks if
the constant proves too coarse.  So a gen2 model is
`ReadAdvance<ReadGPR, -3>` on every GPR read plus MachineScheduler
enabled for that tuning; gen2.5 would add only
`ReadAdvance<ReadGPR, -1, [WriteLoad]>` for its load-use interlock,
which is little to gain for the cost of enabling the pass.

### Branch costs

Measured on the RTL testbenches
([instruction-timing.md](internals/instruction-timing.md)):

| Control transfer | gen1 | gen2.5 |
|---|---:|---:|
| `Bcc` not taken | 2 | 1 |
| `Bcc` taken / `B`, BTB-predicted on gen2.5 | 2 | 2 |
| mispredicted, or indirect `JMP` / return | 2 | 5 |
| `BL` | 3 | — |

gen1 has no prediction: a control transfer is one µop like an ALU
operation.  On gen2.5 the fetch-time BTB (32-entry direct-mapped,
trained by EX: taken allocates, not-taken clears) predicts each direct
branch site's last resolved direction; a correct taken prediction
discards the sequential fetch launched in IF1 as the branch enters
IF2, and a misprediction, indirect jump or return discards the four
slots behind the branch in IF1, IF2, the fetch buffer and ID
([instruction-timing.md](internals/instruction-timing.md#control-transfers-on-penumbra25)).
gen2.5's `MispredictPenalty` is therefore **4**.  gen2 has no BTB, so
every taken branch there costs the redirect; branch behaviour is a
second reason gen2 and gen2.5 cannot share a model.

**A correct prediction still costs a slot.**  The BTB was designed to
make a correctly predicted taken branch free, but it steers IF1 as the
branch enters IF2, by which time IF1 has launched the sequential fetch.
Reaching zero would take launching the target fetch in the same cycle
as the branch — worth weighing for gen3's front end, where every taken
branch in a hot loop pays it.

No scheduling-model field expresses "a predicted taken branch still
costs a cycle", and setting `MispredictPenalty` would change almost
nothing for Penumbra: `SelectOptimize` uses it to choose between a
`select` and a branch, but Penumbra has no conditional-move instruction,
so a `select` becomes a branch diamond either way; SimplifyCFG adds
`TTI::getBranchMispredictPenalty()` (default 0, overridden only by X86)
to its speculation budget, and only for branches marked unpredictable.
The decisions these costs should drive are the branch-versus-branchless
combines — see the "no branch-cost model" entry.

### Other consumers waiting on a feature

- **Multiply cost — landed, as model latencies rather than a
  feature.**  A cycle count is a number, which a feature bit cannot
  carry, and the compiler should know only how long an operation
  holds the core, not how the hardware gets there.  Each scheduling
  model states MUL and DIV latencies; `PenumbraSubtarget::getMulCycles`
  / `getDivCycles` read them, and `getArithmeticInstrCost` and
  `isIntDivCheap()` consume them.  See
  [the constant strength-reduction entry](#compiler-constant-multiply-divide-and-remainder-strength-reduction).
- **Branch-vs-branchless** — the "no branch-cost model" entry above.
  Gate the select-of-constants fold on
  `convertSelectOfConstantsToMath`, answered from the branch costs of
  the tuned generation; `getCFInstrCost` / `getCmpSelInstrCost` can
  read the same costs.
- **Code density** — the "inline small constant-size memcpy" entry.
  Gate the inline-expansion thresholds (`MaxStoresPerMem*`) on I-cache
  pressure, so a small-cache generation keeps the compact `bl memcpy`
  while a larger-cache one inlines.  The same property could later
  skew unrolling and the 16-bit jump-table choice.

### Validation

Lock each knob with lit tests that run one `.ll` under both tune
targets and CHECK the divergence.  The infrastructure test
(`mtune.ll`) pins both the code every name emits identically and a
constant divide that splits by multiply cost, including that `generic`
decides as penumbra2_5.  Cost *numbers* are a measure-on-hardware
follow-up: benchmark
on the ULX3S, not the ISS.

### Open decisions

- Feature granularity: start from the properties that have concrete
  consumers and grow on demand.
- Default-generation policy lives in `generic`'s definition in
  `Penumbra.td` (it follows the most-used variant);
  `PenumbraSubtarget::DefaultCPU` only names `generic`.

## Compiler: fuse a widening multiply into a single MUL_P — RESOLVED

A 32×32→64 widening multiply currently selects to **two** hardware
multiplies (commit `ba7ef55`): the custom legalization of `G_MUL s64`
emits `G_MUL(a,b)` for the low word and `G_S/UMULH(a,b)` for the high,
which select to `MUL`+`MUL_P` (signed) or `MUL`+`MULU_P` (unsigned).
But `MUL_P`/`MULU_P` already produce **both** halves in one instruction,
so the separate low `MUL` is redundant — it recomputes the low product
that `MUL_P` discards into a dead register.  Collapsing the pair to a
single `MUL_P`/`MULU_P` is the remaining optimization.

**Measured leverage (ULX3S @ 25 MHz, mandelbrot seahorse, µs/sample):**
56141 (software `__muldi3`) → 4244 (4-mul schoolbook, `beb685b`) →
2584 (2-mul pair, `ba7ef55`).  Fitting `cost = k·muls + N` to the last
two points gives `k≈830`, `N≈924`, so a 1-mul form projects to
**~1750 µs/sample — a further ~1.47×, ~32× over the software baseline.**
Diminishing: the non-multiply floor (`>>28` realign + escape loop) is
already ~36% of the sample, so the third multiply matters less than the
first two did.  Only multiply-bound fixed-point code (this demo) sees
the full benefit; the kernel and Dhrystone are not multiply-bound.

**Why it's deferred — no clean GISel mechanism exists.**  GISel has no
two-result multiply op (SelectionDAG's `ISD::SMUL_LOHI`/`UMUL_LOHI` have
no `G_*` equivalent), so a single instruction producing both halves can't
be expressed at the generic level.  Prior art doesn't transfer:
- **ARM** (`SMULL`/`UMULL`, our exact twin) forms it via SelectionDAG's
  `SMUL_LOHI` — SelectionDAG only.
- **x86** (`MUL`→`EDX:EAX`) selects each half to a full `MUL` reading the
  fixed result register, then leans on `MachineCSE` to fold the two
  identical instructions into one.  Needs **fixed** output registers;
  our `MUL_P` writes regalloc-chosen vregs, so two `MUL_P`s never CSE.
- **RISC-V RV32** does the same 2-instruction schoolbook we do now and is
  content with it — its `mul`/`mulh` are genuinely two instructions, so
  it has no single-instruction form to fuse toward.

**Implementation routes, both with real cost:**
1. Selection-time pairing in `selectDivMul`: when selecting `G_S/UMULH`,
   scan for a sibling `G_MUL` with identical operands, emit one `MUL_P`,
   route its low output to the `G_MUL` result and erase it.  Order-
   sensitive (either op may be selected first) — the fiddly part.
2. Build `MUL_P` directly in the custom legalization (skip the generic
   pair entirely).  Bypasses the fusion but emits a target instruction
   pre-RegBankSelect, which is non-idiomatic and needs manual reg-class
   constraining.

Not worth the complexity until something multiply-bound matters more than
the ~1.47× on a demo.  Correctness surface if revisited: the same
signed/unsigned/i64 widening + `smul`/`umul` overflow execution tests used
for `ba7ef55`.

**2026-08: the deferral gate has flipped.**  Enabling `BN_LLONG` in the
OpenSSL build (`f0942b3981a9`) made `bn_mul_add_words` — one widening
multiply per limb product — the RSA/DH critical path, and it now runs
the unfused `MUL`+`MULU` pair (~66 divmul cycles where 33 suffice) on
real workloads: RSA sign, DH kex, ssh.  Worse, the compiled loop shows a
**third** multiply per limb: an unfolded multiply-by-known-zero cross
term from the 64×64 narrowing (`mul r11, lr` with `lr` holding a
loop-carried zero, plus a dead `add r7, 0`) that the minimal
`acc + (u64)a*b` repro does *not* reproduce — the zero flows through the
unrolled loop's phis and escapes the constant folds.  Fixing fusion
alone roughly halves divmul time in all bignum inner loops; chasing the
zero-term as well brings ~3× on the multiply component.  Estimated
~1.5–2× on RSA sign end-to-end, on top of the BN_LLONG 1.5–1.8×.

**Fixed via a third route the survey above missed: a target *generic*
opcode.**  Neither selection-time pairing nor pre-RegBankSelect target
instructions were needed — GISel lets a target define its own `G_*`
opcodes (AArch64's `G_ADD_LOW` etc.), which is exactly the missing
"two-result multiply" the generic opcode set lacks.
`PenumbraInstrGISel.td` defines `G_UMUL_LOHI`/`G_SMUL_LOHI`; the
post-legalizer rule `penumbra_mul_to_mul_lohi` fuses a same-operand
`G_MUL` + `G_[SU]MULH` pair (either operand order) into one, and the
selector emits `MUL_P`/`MULU_P`.  Everything stays on the normal
pipeline, so the fiddly selection-order problem never arises.

Gotcha for the next target generic opcode: `isPreISelGenericOpcode()`
covers only the shared `G_*` range, so the selector's entry guard has to
use `MachineInstr::isPreISelOpcode()` — otherwise target generic opcodes
are taken for already-selected instructions and reach the register
allocator unselected (crash in the coalescer, not a clean verifier
error).

Measured statically: 64×64 multiply 4 multiplies → 3; widening 32×32→64
2 → 1; the fractal demos' Q4.28 inner loop 6 → 3.  Divide/remainder was
never affected — `G_SDIVREM`/`G_UDIVREM` already exist generically and
the legalizer's `div_rem_to_divrem` fuses those upstream.

## Compiler: 16-bit jump-table entries when offsets fit

`PenumbraAsmPrinter::emitJumpTableEntry` always emits
`EK_LabelDifference32` entries (`.word target - JT_base`, 4 B
each). For switch tables whose target span fits in ±32 KB the
entries could be 16-bit (`.half`), halving table size in `.text`.

Fix: pre-walk the JT in `PenumbraTargetObjectFile` (or in the
AsmPrinter) computing max signed displacement; switch the entry
encoding to `EK_Inline` 16-bit when the range fits. The runtime
BRJT expansion would need a matching sign-extend before the ADD-
back step (`LDH +SEXT offset,[entry_addr]` → `ADD offset, base`
→ `JMP`). Low-priority cosmetic win — only matters for code-size-
sensitive builds and large switches; for now we always pay the
4 B/entry tax.

## Compiler: named byval args overlap on stack — RESOLVED

When a fixed (non-variadic) function received byval struct args
that spilled past R1-R4, adjacent stack slots overlapped: CC_Penumbra's
`CCAssignToStack<4,4>` reserved only a pointer-sized slot while the
framework's byval-mem path wrote `Flags.getByValSize()` bytes (8 for
`_Complex float`, 16 for `_Complex double`) at that offset.

Resolved by the aggregate-ABI rework above: the `PenumbraABIInfo`
classes route these types so the byval-on-stack path is never taken —
a 5–8 byte aggregate (`_Complex float`) goes Direct([2 x i32]) and a
> 8 byte one (`_Complex double`) goes indirect non-byval with a
clang-materialized copy.  The once-proposed `CCIfByVal<CCPassByVal<4,4>>`
CC change is moot for clang-emitted code.  `complex-7.c` passes and its
exclusion is removed.  The variadic-byval stack-overflow bug
(920625-1.c) was a separate issue, fixed earlier by
`normalizeVarArgByVal()` in `PenumbraCallLowering.cpp`.

## Benchmark: CoreMark-Pro under NetBSD

Dhrystone is the only modern-era integer benchmark in the bare-metal
harness and is widely understood as unrepresentative of real workloads
(tiny working set, no float, no state machine, trivial branching).
CoreMark-Pro is the natural successor: a 9-workload suite covering
JPEG encode, linear algebra (SP float), 64K-point FFT, SHA, DEFLATE,
neural-net inference, parser, and a 125 KB-working-set workload that
meaningfully exercises the TLB.

**Hosting decision: run it under NetBSD, not bare-metal.**
CoreMark-Pro is licensed Apache 2.0 (compatible with project licensing
— same model as our LLVM and NetBSD imports).  EEMBC ship a
Linux/POSIX reference port (`builds/linux/linux32/`) that wires
`th_malloc`, `th_file_*`, `th_thread_*`, `th_time_*` straight to libc
and pthreads.  This is the *canonical* port; the bare-metal
"embedded" port is the lesser-supported one.  Running under NetBSD
dramatically cuts porting work, and the OS-mediated TLB walking and
pmap behavior we'd measure is the path real workloads actually see.
Published industry numbers are almost universally Linux-hosted, so
our methodology lines up with the field on the comparability axis.

Cost is run-to-run noise from scheduler/interrupt/page-fault jitter,
but with single-context (`-c1`), single-user mode, and median-of-N
reporting (the protocol used by pbench in `benchmark/netbsd-bench/`),
noise floor is a few percent — well below the resolution we care
about at current CPU speeds.

**Trademark caveat.**  EEMBC owns the *"CoreMark"* mark; Apache covers
the code, not the name.  Internal scores must be labeled "unverified,
not an EEMBC-submitted score" in `BASELINE.md` and benchmark output
to stay clean of trademark misuse.

**Implementation sketch:**
1. Vendor upstream under `benchmark/coremark-pro/`, preserve LICENSE
   and NOTICE files verbatim.
2. Add `builds/penumbra/netbsd32/` as a copy of
   `builds/linux/linux32/` with cross-compiler invocation tweaked
   (`clang --target=penumbra-unknown-netbsd --sysroot=…`) and
   pthreads omitted (default `num_contexts=1`).
3. Drive EEMBC's Makefile from a new top-level
   `make benchmark-coremark-pro` target, output into
   `build/coremark-pro/`.
4. Extend the image overlay (or wherever the pbench bundling lives) to
   copy the 9 workload binaries plus their input data files (cjpeg,
   parser, zip) into `/usr/local/bin/coremark-pro/`.
5. Capture baseline in `benchmark/coremark-pro/BASELINE.md`, dated
   snapshot + HEAD SHA, following the pbench precedent.

**Prerequisites / risks:**
- libpthread is currently minimal stubs.  Need to verify single-
  context mode links cleanly without pulling in unimplemented
  thread primitives before committing to the build integration.
- FP-heavy workloads (`linear_alg`, `loops-all`, `nnet`, `radix2`)
  will be dominated by soft-float cost on the current CPU.  Numbers
  are still meaningful as a baseline but the suite gets a lot more
  interesting once a hardware FPU lands.
- `radix2-big-64k` walks ~512 KB of complex floats — blows past our
  1 KB caches and 64-entry TLB and is the most interesting workload
  from a memory-hierarchy perspective.

**Related, separate item.**  CoreMark (singular, not -Pro) is a
better fit for the *bare-metal* harness — single C file, no float,
no file I/O, drops cleanly alongside dhrystone in `benchmark/`.
Plausible to land it first as the dhrystone successor in the
bare-metal tier, then add CoreMark-Pro to the hosted tier on top of
pbench.  The split gives a clean two-tier story: bare-metal
benchmarks measure the CPU in isolation, hosted benchmarks measure
the system.

## Kernel/compiler: dedicate R12 (TP) to curlwp — DONE

The ABI reserves R12 as the thread pointer, and the kernel uses no TLS,
so R12 sits unused in kernel code.  Pinning `curlwp` there — the way the
NetBSD RISC-V port pins it in `tp` via
`register struct lwp *riscv_curlwp __asm("tp")` — turns every `curlwp`
read from a load of `cpu_info_store.ci_curlwp` into a register read.  It
costs no register pressure: R12 is already reserved, so the allocatable
set does not change.

### Compiler support — DONE

Two pieces, both landed:

- `register T x __asm("r12")` works end to end (`ff46f87450d8`):
  `getRegisterByName` resolves the canonical name and the ABI aliases,
  the GlobalISel legalizer lowers `G_READ_REGISTER` /
  `G_WRITE_REGISTER` to a COPY to/from the physical register, and
  naming an allocatable or unknown register is a fatal error.  clang
  already lists `r12` in `GCCRegNames`.
- The across-call spill problem is fixed (`32937b8bf9d7`): R12 is now a
  *member* of `GPR_Allocatable` (listed last, still excluded from
  allocation by `getReservedRegs` — the RISC-V `tp` idiom), so the
  register coalescer substitutes R12 into cross-call `%v = COPY $r12`
  values and each use gets a fresh `mov rX, r12` instead of a
  callee-saved copy or stack spill.

### Kernel wiring — DONE (`b625d49e43af`)

curlwp is read from R12, scoped to `_KERNEL` in `cpu.h` (`_KMEMUSER`
keeps the memory-load macro so libkvm consumers are unaffected).  Four
boundaries keep R12 == curlwp whenever kernel C runs:

1. `locore.S` sets R12 = `&lwp0` before the first C call at boot.
2. `cpu_switchto` already saved/restored R12 via `pcb_context` (like
   SP/LR), so it threads across voluntary switches unchanged; it also
   already writes `ci_curlwp`.  No change was needed there — the
   originally-planned "additionally set R12 = newlwp" is redundant.
3. `cpu_lwp_fork` seeds the child's `pcb_context` R12 slot — the
   `*pcb2 = *pcb1` copy would otherwise leave the parent lwp there.
4. `_trap_common` reloads R12 from `cpu_info_store` after the trapframe
   save, before running C.  A userland trap arrives with R12 holding the
   user TLS pointer, which the trapframe save/restore preserves; the
   reload is unconditional (correct from both entry modes, cheaper than
   branching).

`cpu_info_store.ci_curlwp` stays the canonical copy — the trap-entry
reload and any cross-lwp reads still consult it.

### Measured benefit

A `curlwp->field` access drops from `lli`+`lui`+`ldw`+`ldw` (4) to a
single `ldw [r12+off]` — the load takes R12 as its base directly, so it
is 4→1, better than the 4→2 originally estimated — and survives calls
for free (R12 reserved).  Same-session pbench A/B on the ULX3S (min;
identical clang and rootfs, only the kernel source differing, libc rows
flat to confirm isolation): `getpid` 122.49→118.53 µs (−3.2%),
`clock_gettime` 227.33→197.90 µs (−12.9%, with much lower variance).
fork/`pipe_pingpong` are within noise — curlwp is a negligible fraction
of their millisecond-scale cost, and their swings track 1 KB
direct-mapped I-cache layout (the kernel is 8 KB smaller), not this
change.

## Hardware + kernel: local console (HDMI display + USB keyboard)

Design complete and committed as docs; implementation underway on both
halves — per-layer status in the subsections below. The goal is a
standalone local console — character-cell video out over GPDI/HDMI and a
USB keyboard in — so the machine needs no host terminal. NetBSD is the
first consumer (boot may stay on UART initially); a boot-ROM local
console is a later, well-defined follow-on.

Two new autoconfig device classes
([`system/bus.md`](system/bus.md)):

- `CLASS_DISPLAY` (6) — display adapter: a character-cell console as
  the mandatory core, plus an optional framebuffer capability
  (`CAP.FRAMEBUFFER`, geometry/format discoverable). Contract
  [`system/devices/display.md`](system/devices/display.md),
  microarchitecture [`internals/display.md`](internals/display.md).
- `CLASS_USBHC` (8) — transaction-level USB host. Contract
  [`system/devices/usb-host.md`](system/devices/usb-host.md),
  microarchitecture
  [`internals/usb-host-controller.md`](internals/usb-host-controller.md).

Decisions already settled (rationale lives in the docs, not here):

- Text cells are 16-bit `{glyph, attribute}` in 32-bit-strided slots;
  color is optional/discoverable (`CAP.COLOR`); the hardware cursor is
  mandatory; mode 0 (640×480 / 80×30) is the mandatory power-up mode for
  monitor compatibility, with extra modes optional via `MODE_SEL`.
- USB exposes a transaction-level minimum (token / data / handshake); the
  NetBSD HCD sits under the MI USB stack (`dev/ic/sl811hs.c` as the
  structural template), which owns enumeration + HID. `ukbd` → `wskbd`
  and a `pcdisplay`-style `wsdisplay` meet at `wscons`.

Implementation work, by layer:

### Display RTL, text path — first cut: mode 0 only

Bring-up order: prove the output path on glass with the test-pattern
generator before any machine-side integration. Everything up to the
GPDI connector is CPU-independent, so the physical layer's real risks
(PLL config, serializer word alignment, lane order) are isolated from
bus/autoconfig work and verified with nothing but a monitor. Machine
integration then lands against a display path that is already known
good.

1. Pixel-domain chain — done: timing generator (`video_timing`),
   test-pattern generator (`video_pattern_gen`), TMDS encoder
   (`video_tmds_encoder`), each unit-tested against an independent
   oracle in `make test-modules`.
2. 10:1 DDR serializer — done: `video_serializer`, 5x serial clock
   from the same PLL, two bits per serial cycle into an `ODDRX1F`
   owned by the board top (pure-logic module — the sdram_ctrl /
   sdram_phy_ecp5 split, one level down). The DVI clock lane is a
   fourth instance fed the constant word `10'b0000011111`, so clock
   and data share one launch path by construction.
3. Closed-loop chain testbench — done: timing → pattern → three
   encoders + clock lane → serializers, with the testbench playing the
   monitor: sample both DDR phases, character-align on the blanking
   control codes, TMDS-decode, and compare a full frame against the
   pattern oracle. With this passing, only the PLL primitive and the
   pads remained unproven.
4. Bring-up top — done: `ulx3s_video_test_top` (via the `TOP=` escape
   hatch — a probe, not a registered machine) instantiates the
   chain-test module in front of the pieces simulation could not
   prove: a dedicated video EHXPLLL at 25 MHz pixel / 125 MHz serial
   (integer PLL ratios cannot make VESA's 25.175 MHz from the 25 MHz
   crystal; the resulting ~59 Hz refresh is within monitor tolerance
   and the norm on this board) and the `ODDRX1F`-driven `gpdi_dp[3:0]`
   pads. Validated on glass: a stable test pattern, monitor syncs at
   the even-25 MHz timing. The triage ladder (no signal → PLL/clock
   lane; rolling → word alignment; wrong colors → lane order) lives in
   the top's header comment.
5. Pixel generator — done: font ROM (`video_font_rom`, generated from
   NetBSD's public-domain bold8x16 via `wsfont2hex.py`), dual-clock
   cell RAM (`video_cell_ram`), and the 4-stage scan-out pipeline
   with the blinking hardware cursor (`video_textgen`), each
   unit-tested against an independent oracle; `video_text_chain`
   closes the loop through the TMDS link, and the bring-up top shows
   the preloaded splash (`hw/video/splash.txt` → `text2cells.py`,
   UTF-8 → CP437) on glass.
6. Machine side — done: `video_display` (register block + CELLS
   aperture + 2-FF control crossings around the text chain) behind an
   `autoconfig_dev` wrapper (`CLASS_DISPLAY`, 16 KiB), wired into
   `ulx3s_penumbra1_top` with a dedicated video PLL and validated
   from the ROM monitor — autoconfig discovery, register dumps, and
   cells written over the bus rendering on the monitor. The display
   hardware ladder is complete; what remains of the local console is
   software (`wsdisplay` back-end below) and the USB keyboard half.

**Timing after the display landed.** The gen1 board top now sits on
the 25 MHz constraint rather than above it: a seed sweep spans
23.99–27.5 MHz with only half the seeds passing, so placement luck —
not logic depth — decides each build. The critical path is unchanged
by the display (the datapath → `status_reg` ESR cone, ~41.6 ns
either side of the change, with identical LUT/FF/EBR counts), and the
display's own domains have enormous margin (pixel 85 MHz against 25,
serial 339 against 125). `keep_hierarchy` on `video_display` and the
pinned seed hold it for now; the durable fix, when the margin is
wanted back, is a floorplan for this top (`LPF_DESIGN` + `PREPACK`,
as `ulx3s_penumbra2_top` already carries) or shaving the ESR cone.

Goal (later): PLL dynamic-reconfig FSM + mode 1 (800×600 / 100×37);
EDID/DDC readout when the mode list gives it a consumer (contract
space — CAP bit, `EDID_CTRL`, the `EDID` aperture — is reserved, and
the ULX3S already routes the GPDI DDC pair to FPGA pins).

### Display RTL, framebuffer capability — done

`CAP.FRAMEBUFFER` is implemented and validated on glass from the ROM
monitor: a 320×240 8bpp picture pixel-doubled onto the mode-0 raster,
its 256-entry palette, and the source select at the parallel-RGB seam.
Selecting it never re-times the output, so it needed nothing from the
mode/PLL work.

- Storage is `video_fb_ram` (per-byte write enables, since the aperture
  is mapped into a rendering process) and `video_fb_palette`; scan-out
  is `video_fbgen`. The output chain carries both sources and is named
  `video_pixel_chain` for it.
- Scan-out addressing is a counter, not an offset computation:
  framebuffer lines are contiguous, so a line's walk ends on the next
  line's first word, repeating a line reloads the start it began from,
  and the wrap at the end of the array is the frame rewind. No adder,
  no stride constant, and an address that cannot leave the array.
- Cost measured on `ulx3s_penumbra1_top`: 42 EBR (40 pixels + 2
  palette), bringing the top to 128/208. The estimate of ~34 in the
  microarchitecture notes assumed ideal bit packing; byte-wide banks
  waste the ninth bit of each block and round depth up.
- The device window grows to 256 KiB, and the display becomes the first
  autoconfig device here to consume byte enables.
- Timing after it landed: gen1 closes on 9 of 10 placement seeds, the
  pinned seed 5 at 28.31 MHz against the 25 MHz constraint.

Remaining is software: the `wsdisplay` dumb-framebuffer path in
`pdisplay` (`GINFO` / `GET_FBINFO` / `LINEBYTES`, `PUTCMAP` onto
`FB_PALETTE`, `SMODE` driving `FB_SEL`, and an `mmap` bounded to the
pixel aperture so a mapped client cannot reach the control registers).

### USB host RTL — low + full speed
- `usb_crc5` / `usb_crc16` — token and data CRC generators, the
  board-neutral MAC's first leaf cells (Galois LFSRs; CRC5 bit-serial,
  CRC16 byte-parallel for the one-byte-per-cycle datapath). The bare
  modules emit the remainder; the MAC adds the on-wire complement +
  bit-reverse. Reference-checked unit tests + a `sw/tools/usb_crc.py`
  model (`6251a87`, `b74a5ef`).
- `usb_bit_stuff_tx` — SIE transmit bit-stuffer: inserts a 0 after six
  consecutive 1s, with a consume-handshake (`o_stuff`) that back-pressures
  the serializer. Reference-checked unit test (`ca1b502`). Per-packet
  `i_init` re-seeds the run count with SYNC's terminating 1, the
  unstuffer's mirror (`768a244`).
- `usb_nrzi_encode` / `usb_nrzi_decode` — SIE line coding: a 0 toggles the
  line level and a 1 holds it (the decoder is the exact inverse). Each
  reference-checked; the decoder round-trips the encoder (`d2b915d`).
  Made line-true for chain composition (`de8bd71`): the encoder gains a
  per-packet `i_init` reloading its idle-J reference and holds `o_line`
  registered for a full bit time; both cells reset to idle J — a K-shaped
  seed makes the first post-reset sample decode as a phantom 1, which is
  SYNC's end marker.
- `usb_bit_unstuff_rx` — SIE receive bit-unstuffer: removes the stuffed 0
  after six consecutive 1s (`o_valid` drops on it) and flags a bit-stuff
  error (a 1 where the 0 was due → seam `o_rx_error`) in the same decision.
  Round-trip + error-injection unit test (`65dfccd`). Per-packet `i_init`
  re-seeds the run count with SYNC's terminating 1 (USB counts it), and
  `o_valid`/`o_error` are qualified by `i_en` for sparsely-paced
  consumers (`9f12a7a`).
- `usb_serialize_tx` — SIE transmit serializer: shifts each byte out LSB
  first across a byte handshake (`o_byte_ready`) above and the bit-time
  tick below, folding the stuffer's back-pressure in as `i_hold` so an
  inserted stuff bit holds the current bit instead of dropping it; goes
  empty on the final bit to keep the line bubble-free between bytes.
  Unit test across five pacings (gated tick + injected hold) (`dc8a57c`).
- `usb_deserialize_rx` — SIE receive deserializer, the serializer's
  mirror: shifts recovered bits in at the MSB so eight assemble into an
  LSB-first byte; `i_valid` ignores removed stuff 0s and `i_init`
  re-aligns byte boundaries per packet (a concurrent assertion guards
  that `i_init` never rides a data bit). Mirror-checked against the
  serializer's bit order + a cross-packet realignment case (`b9e9a8e`).
- `usb_oversample_rx` — SIE receive clock recovery: oversamples the
  resolved J/K line (5×/40× from `usb_pkg`) and locks a per-bit phase
  counter to NRZI edges, strobing the sampled level at each bit midpoint
  into `usb_nrzi_decode`. The edge lock re-centers to phase 1 and
  suppresses a sample that coincides with an edge, which is what holds
  lock at the 5× full-speed margin. `usb_pkg.sv` lands here for the
  PHY-internal constants (divisors, speed, J/K/SE0). Waveform-recovery
  unit test at both speeds ± edge jitter (`cfb3f07`).
- `usb_line_state` — SIE differential decode: resolves the D+/D- pair
  into SE0 / J / K / SE1, with the speed-dependent J/K swap (J is D+ high
  at full-speed, D- high at low-speed). Feeds the sampler its J/K symbol
  and the framing layer its SE0. Exhaustive unit test (`b72f044`).
- `usb_rx_framing` — SIE receive packet bracketing: SOP on the first K
  out of idle, SYNC end on its terminating 1 (an end-marker, not a
  full-pattern match, so hub-stripped SYNC still detects), payload
  gating to the unstuffer, EOP on SE0. Unit test incl. stripped SYNC +
  back-to-back packets (`6ac3503`).
- `usb_rx_test` — receive-chain integration DUT (line state →
  oversample → NRZI → framing → unstuff → deserialize), the shape of
  `usb_phy_<target>`'s receive half. End-to-end test drives raw D+/D-
  packet waveforms and requires the payload bytes back: both speeds,
  edge jitter, hub-stripped SYNC, stuffing edge cases, back-to-back
  packets (`6ac3503`).
- `usb_tx_framing` — SIE transmit packet bracketing, `usb_rx_framing`'s
  mirror: owns the bit-clock pacer (5×/40× divider — transmit generates
  timing, receive recovers it), feeds SYNC to the encoder, seeds the
  stuffer on SYNC's last bit, pumps payload until the serializer empties
  *and* the stuffer owes nothing (a packet ending in six 1s still gets
  its trailing stuff bit), then drives the SE0/SE0/J EOP raw below the
  encoder and releases. Unit test: phases, spacing, trailing stuff,
  re-arm (`1bba2b9`).
- `usb_tx_test` / `usb_loop_test` — transmit-chain integration DUT
  (serializer → stuffer → framing/encoder → line mux → registered pins,
  the shape of `usb_phy_<target>`'s transmit half) with a structural
  waveform testbench (SYNC pattern, exact stuffed payload, EOP shape),
  and the TX→RX loopback closing the loop over every SIE cell: bytes fed
  in come back out, both speeds, multi-packet (`1bba2b9`).
- **The SIE is complete**: all bit-, timing-, framing-, and line-level
  cells built, unit-tested, and proven end-to-end in both directions.
- Seam verified against the UTMI+ spec rev 1.0 (saved locally as
  `~/UTMI-PLUS-SPECIFICATION.pdf`) so a real ULPI/UTMI PHY satisfies it
  unmodified: opmode = the UTMI+ operational modes; bus reset = the
  HS-termination drive state (`xcvr_sel 00` + `term_sel 0`, SE0 by
  electrical result — FS/LS-only PHYs drive SE0 directly, caps carve-out
  documented); resume = opmode 10 + held `00h` data; LS keep-alive = the
  transceiver-decoded one-byte `A5h`; `line_state` = raw {D-, D+} with
  SE0 glitch filtering; `usb_speed_e` now carries the XcvrSelect
  encoding end to end (`6959ad2`, `0cba7d2`). `XFER_STATUS` gained
  `RXTOGGLE` — hardware ACKs CRC-good IN data and reports the received
  toggle; retransmission detection stays in software (`6959ad2`).
- **MAC — in progress.** Decomposition agreed, leaf-cell style, all on
  the 60 MHz clock (the CDC crosses later), build order as listed.
  Done: `usbhc_pkt_tx` — token/SOF/DATAx/handshake transmit, the
  on-wire CRC complement+reflect above the bare CRC cells, the bare-PID
  keep-alive transmit; PID codes in `usb_pkg`; golden-vector testbench
  anchored to `sw/tools/usb_crc.py`, including byte-per-cycle pacing
  (`0932a66`). Done: `usbhc_pkt_rx` — PID classify + check-nibble,
  packet body → buffer as received (CRC bytes included, per the DATA
  contract), CRC16-residual check, EOP-latched `{pid, len, ok, err,
  overflow}` (`87c0a57`). Done: `usbhc_txn` — token → [data] →
  handshake FSM, speed-scaled turnaround timeout + drain-aware
  inter-packet gaps, hardware host-ACK, trust-then-dispatch RESULT
  classification, IN-only rx→buffer store grant; TOKEN.PID and RESULT
  encodings in `penumbra_pkg` (`354e06f`). Done: `usbhc_frame` — 1 ms
  timer, FRAME counter, SOF/keep-alive request under the MAC arbiter's
  grant (never splits a transaction), missed markers coalesced to the
  latest frame number (`101ab5b`). Done: `usbhc_port` — debounced
  connect/speed detect from the raw line (D+ pull-up ⇒ FS, D- ⇒ LS,
  SE1 = fault), detection paused while the host drives the bus,
  reset/resume recipes, PORT_CHANGE events (`8635b75`). Done:
  `usbhc_mac` — the five-cell composition: transmit arbiter (a start
  beats a due marker, the grant follows the marker request down, and
  launches wait out a disowned marker's drain), owner-routed
  packet-transmitter mux and completion strobe, resume transmit
  override, IN-grant gate on buffer stores. The MAC-level testbench
  drives the seam at line-realistic pacing against
  `hw/sim/usb_device_sim.h` — the `UsbDeviceSim` embryo both sims will
  share (its future keyboard input must NOT take terminal stdin — the
  UART console owns it — separate pty/socket, mechanism TBD)
  (`63b0bf3`). **The MAC is complete.** Next: the swappable PHYs,
  `usb_phy_sim` / `usb_phy_ecp5`.
- `usb_phy_sim` / `usb_phy_ecp5`: compose the SIE cells behind the seam
  (`usb_rx_test`/`usb_tx_test` are the two halves' shapes); `phy_sim`
  replaces the line layer with a byte-level packet port + connect/speed
  sideband exported through `machine_sim` like the SPI/SD model, pacing
  bytes at real bit-time rates so MAC timing stays honest.
  Done: `usb_phy_ecp5` — the SIE composition plus the PHY-tier glue:
  drive-state decode for the signaling recipes (reset SE0, resume raw K
  + appended LS EOP, non-driving release), the transceiver-decoded LS
  keep-alive (lone A5h ⇒ bare EOP), receive squelch (own transmissions
  never reach the MAC; the raw line-state sideband still sees them), the
  SE0 transition-glitch filter. Pair testbench cross-wires two instances
  over a resolved bus with cycle-counted recipe waveform checks
  (`2e602f6`); the oversampler re-locks on live speed changes to survive
  the port's transceiver-code swaps (`72c6a72`). Done: `usb_phy_sim` —
  the seam with the line layer replaced by a byte-level device port:
  packet-byte pulses out (EOP/keep-alive/reset/resume as explicit
  strobes and levels), a credit handshake in (the device model owns its
  turnaround; the PHY owns bit-time pacing), connect/speed as the
  pull-up sideband. The stack testbench mates `usbhc_mac` to
  `usb_phy_sim` with `UsbDeviceSim` behind the device port — the seam
  proven tier-to-tier ahead of machine integration; the machine_sim
  export of the device port rides with the `usbhc_regs`/autoconfig step
  (`6702220`). **The PHY tier is complete.**
- Done: US2 wiring — the gen1 board top carries the machine_sim USB
  block with `usb_phy_ecp5` on the seam; D+/D- sensed and driven
  single-ended on the `usb_fpga_bd` pads (the differential-input
  pair on the same copper stays unused, so the vendor LPF is
  untouched), pulls on `usb_fpga_pu_*`, and the PLL's spare CLKOS3
  serving the 60 MHz USB clock exactly (build-failure guard on VCO
  retargets). Validated on hardware: a real full-speed device
  enumerates end to end through the NetBSD stack — descriptors,
  SET_ADDRESS, serial string — on the physical port.
- `autoconfig_dev` wrapper (`CLASS_USBHC`); machine_sim integration test
  (poll CONNECT → reset → GET_DESCRIPTOR → R1) under `make test`.
  Done: `usbhc_regs` + `usbhc_cdc` + the `usbhc` composition, proven
  from the bus at a 5:2 clock ratio by `tb_usbhc_dev` — the executable
  spec for the ISS model (`ebc65e7`). Done: the gen1 machine attachment
  and `test_usbhc_enum` — autoconfig discovery through a full
  enumeration (descriptor read, SET_ADDRESS with the old address proven
  dead, wLength-bounded re-read, STALL for an unknown request) with
  self-identifying failures (`867aa7d`). Bringing the first bare-metal
  software onto the autoconfig path exposed a pre-existing config-space
  write-stall fault the ROM's probe handler had been swallowing
  (`aa65aff`).
- Done: **ISS `CLASS_USBHC` at register level** — the register contract
  (autoconfig + TOKEN/XFER/PORT/FRAME/DATA) in `sw/sim/penumbra_iss.cpp`
  behind a two-slot autoconfig chain, transaction-level only (no
  SIE/timing model), with the shared `UsbDeviceSim` enumerating behind
  it; `test_usbhc_enum` runs under `make test-iss` (`140db78`). Driver
  bring-up (the NetBSD HCD, a ROM keyboard reader) iterates on the fast
  simulator now. The keyboard-input-not-on-stdin constraint applies when
  the HID personality lands.

### Kernel
- `ukbd` + `wskbd` binding for HID keyboards.  The host-controller
  driver, the MI USB stack, and `uhidev`/`uhid` are done — keystrokes
  reach userland through `/dev/uhid*` — but a `wscons` console needs
  the keyboard bound as a `wskbd` input instead.
- Done: `wsdisplay` back-end for `CLASS_DISPLAY` (`pdisplay`, emulops
  over the `CELLS` aperture with a RAM shadow for the copy ops) plus
  the keyboard half — `ukbd` → `wskbd` → the display's kbdmux. Proven
  on hardware: keystrokes reach `ttyE0` and echo on the monitor, so
  the local console is a working terminal. `wsconsctl` state, the
  `MODE_MAPPED` mmap path, and the `EXTGLYPHS` range beyond ASCII
  remain unexercised.
- Local console, remaining: a session on `ttyE0` at boot needs
  multi-user (`init` reading `/etc/ttys`, whose entry is in place) —
  today a getty must be started by hand from the single-user shell.
  Making the display the *kernel* console (boot messages on glass, DDB
  on the local keyboard) additionally needs `wsdisplay_cnattach` plus
  a polled console keyboard: `ukbd_cnattach` requires interrupt-pipe
  polling in `pusbhc`, without which the console would have output but
  no input between attach and USB enumeration.

### Boot ROM — later
- Per-class console backends (text-cell writes; framebuffer software
  glyphs once that class lands) and a USB boot-keyboard reader, behind a
  putc/getc abstraction the monitor selects at autoconfig.

## Kernel: networking — first NIC through the USB host controller

Decision: the first network adapter is a **USB NIC on the CLASS_USBHC
port**, not a dedicated NIC device. Rationale: the HCD is already owed
for the local-console keyboard, and once it exists the whole MI USB
stack plus every in-tree USB Ethernet driver (`axe`, `aue`, `cdce`, …)
comes along — no new RTL, no device firmware, no from-scratch NIC
driver. The alternatives stay viable later: the ESP32 SLIP bridge
([`system/devices/esp32-nic.md`](system/devices/esp32-nic.md), needs
RTL + ESP-IDF firmware, adds WiFi) and a Wiznet W5500 on the existing
SPI class (driver-only, needs external hardware). Both have a lower
throughput ceiling than software-driven full-speed bulk, so USB-first
costs nothing on performance either.

Facts the plan leans on:

- Bulk transfers need no controller support beyond the minimum
  protocol: interrupt-vs-bulk is host scheduling policy, and the
  transaction primitives (token / data / handshake, 64-byte buffer)
  are identical. Done: `usb-host.md`'s scope now says so — the three
  handshaken transfer types are the minimum; isochronous / hubs /
  DMA stay behind `CFG_ID`.
- NetBSD's `slhci` (`dev/ic/sl811hs.c`) proves the whole MI stack —
  bulk endpoints included — over exactly this controller shape:
  FS/LS, one software-driven transaction at a time. It is the
  structural template.
- The cost profile is per-transaction software round trips; bulk-IN
  polling pays for NAKs. Usable, not fast — order 100–300 KB/s.
- Device caveat: HS-only adapters may enumerate at FS but offer no
  usable function; safe first targets are USB 1.1-era chips (`aue`,
  `axe`, `url`, `kue`) or a CDC-ECM gadget (`cdce`), which is also
  the cleanest personality to model in simulation.

Work items, in order:

- **ISS/device-model: complete enumeration.** Done: `UsbDeviceSim`
  serves configuration and string descriptors, latches
  `SET_CONFIGURATION`, and terminates exact-multiple control reads
  with the required ZLP; pinned by the grown `test_usbhc_enum` on
  both simulators (`bd53ce1`).
- **Device personalities, in order:** first **mass storage** —
  Bulk-Only Transport plus the small SCSI subset (`INQUIRY`,
  `READ CAPACITY`, `READ(10)`, `WRITE(10)`, `TEST UNIT READY`,
  `REQUEST SENSE`) backed by a host file image, the `+sdcard=`
  pattern. It exercises bulk in both directions with hard data
  integrity (mount a filesystem through it) and needs no host-side
  network plumbing. Done: the wire layer grew a device-function
  seam — `UsbFunctionSim` carries the config descriptor, class
  control requests, packet-level bulk-out, and pull-model bulk-in,
  while `UsbDeviceSim` keeps toggles, chunking, ZLP termination,
  and halt state — and `UsbMassStorageSim`
  (`hw/sim/usb_msc_sim.h`) implements the transport + SCSI subset
  over it, attached in the ISS via `+usbdev=disk:<img>`/`USBDEV=` — the
  generic device-spec argument personalities share — and
  hot-pluggable at runtime with `Ctrl-A U` (kernel detach and
  re-enumeration verified). The
  CDC-ECM NIC personality (bridged to a host TAP device) follows —
  raw packet access is the only part of the network path that is
  host-environment-dependent.
- **NetBSD HCD** — the `CLASS_USBHC` driver from the kernel section
  above, iterated on the ISS. Done: the bus-methods layer —
  `usbd_bus_methods`, software root hub under `usbroothub`, port
  power/reset recipes, the connect-change latch, and the root-hub
  interrupt pipe; `uhub0` explores the port, resets, and walks up to
  the device transfer pipes (`37cc42c`). Done: the transfer pipes —
  a one-transaction-at-a-time engine over TOKEN/XFER_CTRL/DATA with
  a per-pipe state machine (control SETUP→DATA→STATUS, chunked
  data stages, software toggles with RXTOGGLE retransmission
  discard, forced-ZLP OUT endings, three-strikes error retry) and a
  ready-queue scheduler: interrupt endpoints launch at `bInterval`
  on gated SOF interrupts — every launch, not only NAK retries —
  control/bulk NAKs retry round-robin
  (degenerating to immediate relaunch when alone); completions
  post from the hard interrupt to the soft interrupt under the MI
  `usbd_xfer_trycomplete` discipline, and aborts detach at any
  engine state, orphaning an in-flight transaction (safe: all data
  stages through the controller's DATA buffer). `uhub0` now
  enumerates the sim device end to end — descriptors, SET_ADDRESS,
  strings — to a clean "not configured" report on the ISS.
  Done: the kernel config attaches `umass`+`scsibus`+`sd`
  (`files.scsipi` include, `sd` device majors); an FFS filesystem
  on the simulated USB disk mounts and round-trips file data on
  the ISS — bulk verified in both directions with hard integrity.
  Networking landed through `ure(4)` on a real RTL8152 rather than
  the `cdce` simulation path: `ifconfig` plus the INET stack carry
  sustained traffic without loss.  HID landed as `uhidev`/`uhid`.
- **Hardware: US2 wiring** — Done (tracked in the console section
  above): VBUS sourcing confirmed on the board, and commercial
  devices — mass storage, HID keyboards, a Realtek RTL8152 NIC,
  and a cascaded hub — all work on the physical port.

## Kernel: bus_space stream methods when a consumer driver arrives

The MD bus_space layer implements exactly the operations the current
driver set uses; unimplemented ops fail at link (orphan
`bus_proto.h` prototypes), never silently. One conditional MI code
path exists beyond that contract: drivers in the `wdc` family
(`ata_wdc.c`, `atapi_wdc.c`, `wdc_pcmcia.c`, `siisata`) and several
NICs test `__BUS_SPACE_HAS_STREAM_METHODS` and fall back to the
non-stream `multi_N` ops when it is absent. On a little-endian-only
machine the fallback is semantically identical, so today's absence
is benign — but if such a driver ever enters the config, define the
`*_stream_N` variants as trivial aliases of the non-stream ops plus
`__BUS_SPACE_HAS_STREAM_METHODS 1` in `bus_funcs.h`, so the driver
takes its intended path explicitly rather than by fallback. Same
trigger discipline as the display console's `region_2`/`copy_region_2`/
`set_region_2` additions: implement when the consumer exists to
test against, not speculatively.

## Hardware: USBHC v2 — multi-transaction transfers, NAK pacing, PRE

Three planned upgrades to the transaction engine.  The first two target
the two
dominant software costs of the one-transaction-at-a-time design:
every 64-byte transaction costs one interrupt plus a PIO drain or
refill of the DATA window, and a NAKing endpoint costs an interrupt
per poll.  Full-speed bulk fits ~19 packets per frame (~1.2 MB/s);
per-transaction interrupt handling caps a 25 MHz CPU well below
that, and the interrupt tax is felt system-wide (binary IPL, single
wire-OR'd line).  Both features remove interrupts rather than add
wire bandwidth.

- **Multi-transaction transfers.**  Grow the DATA buffer (one 18 Kb
  BRAM covers 2 KB; the CAP register already advertises the size and
  the driver honors it) and let the engine chain max-packet
  transactions within one transfer: keep issuing tokens while ACKs
  continue and buffer remains; stop on buffer-full, short packet,
  NAK, or error.  One completion interrupt per transfer chunk — a
  512-byte disk block or a ure RX aggregate completes as one event
  instead of 8–32.  Toggle tracking for the chained run moves into
  hardware (software seeds it per transfer); XFER_STATUS grows a
  byte count, short-packet flag, and terminating result; the frame
  timer must gate chain segments that cannot finish before EOF.

- **Hardware-assisted NAK retry.**  On NAK, re-arm the latched
  transaction at the next frame tick (optionally a per-transaction
  interval field for interrupt endpoints) instead of raising
  XFER_DONE; interrupt only on a non-NAK outcome or a software
  cancel.  An idle interrupt endpoint or a not-ready bulk device
  then costs zero interrupts.  Open design points: the cancel
  handshake (cancel-versus-completion race), and port sharing — a
  parked retrying transaction occupies the single engine, so either
  software preempts it around other traffic or the engine grows a
  second periodic-only slot (the ISP1362 INTL/ATL shape, scaled
  down).

- **PRE packets — low-speed devices behind a hub.**  The one known
  functional gap rather than a performance one.  A full-speed hub
  forwards to a low-speed port only when the host prefixes the
  transaction with a PRE packet sent at full speed and then signals
  the transaction itself at low speed, so the transmitter must
  switch bit rates mid-transaction and the receiver must expect a
  low-speed reply on a full-speed segment.  Direct-attached
  low-speed devices already work; today an older keyboard that
  works on the port goes silent behind a hub.  Wanted for the
  exhibit topology, where everything hangs off one powered hub.

Both must ride the device contract compatibly: new capability bits
and control fields that old kernels never set, with the existing
single-transaction register behavior as the default.

## NetBSD: ure(4) RX latency quantized at ~100 ms on RTL8152B

A lone received packet (LAN ping reply) is delivered ~100 ms after
it reaches the adapter; sustained traffic is unaffected.  The
suspected mechanism is the chip's RX aggregation flush policy: a
bulk-IN transfer only completes on a short packet, and the device
holds a partial aggregation buffer until an internal flush
threshold.  The driver programs `URE_USB_RX_BUF_TH` with
`URE_RX_THR_HIGH` and never touches the flush timing.  An
experiment writing `URE_RX_THR_SUPER` was inconclusive because the
transport was unreliable at the time.  It no longer is: the
receive-framing, packet-fusion, response-timeout, and pull-down
defects that produced dead sessions and sporadic transaction errors
are fixed, and the link now runs thousands of packets without loss,
so the latency quantum is a clean target.  Progress needs the
RTL8152B register documentation for the threshold field.  Any
resulting driver fix belongs upstream in the MI driver, not carried
as a local patch.

## NetBSD: outbound ping sees ~3x the latency of inbound replies

Pinging the board from another host gets replies in ~25 ms; pinging
out from the board reports ~80 ms round trips on the same link.  The
inbound reply is generated entirely in the kernel (softint echoes
the ICMP request), while the outbound measurement runs through
ping(8): a userland send, a process sleep, and a wakeup plus
copyout on the reply.  The delta therefore bounds the
kernel-to-userland round trip, and it is in the same league as the
measured cv_wait round-trip cost, pointing at scheduling/wakeup
latency rather than the network path.  Worth separating the two
suspects — wakeup latency versus receive-path copy cost — with the
syscall microbenchmarks before touching anything; a cheap
discriminator is timing a local UDP echo against localhost, which
keeps the USB adapter out of the loop entirely.

The USB debug and correctness campaign (line capture, input
synchronizers, arbiter drain and fairness, SOF-delivery counter)
consumed the gen1 top's historical timing slack: seed sweeps that
used to reach 28–30 MHz now plateau at ~25–27, with the CPU's
single-cycle cone still the critical path — the USB logic pressures
its placement rather than appearing in it.  Per the
floorplan-after-features policy, give the USB subsystem the
deliberate treatment: keep_hierarchy over the usbhc/PHY cluster
(the capture module already has it) and, if needed, a REGION
constraint pinning the USB complex away from the CPU cone, then
re-sweep for a pin at or above the 27 MHz floor.

## Kernel: soft interrupts take the scheduler path

The port does not define `__HAVE_FAST_SOFTINTS`, so every soft
interrupt goes through the generic implementation in
`sys/kern/kern_softint.c`.  There `softint_trigger` only flags a
reschedule; the handler runs in its own thread when the scheduler
next switches, and each dispatch costs two full switches
(`mi_switch` plus `cpu_switchto`), into the softint thread and back
out.  When that happens depends on what the CPU was doing:

- **Idle:** promptly, after those two switches.
- **Running a user process:** at its next return to user mode,
  where the pending reschedule preempts it onto the run queue.
- **Running kernel code:** not until that code sleeps, reaches a
  preemption point, or returns to user mode.  There is no kernel
  preemption, and `kern_softint.c` refuses `__HAVE_PREEMPTION`
  without fast softints.

With fast softints the handler runs as the interrupt exits, on a
context borrowed from the interrupted LWP: one stack switch and a
callee-saved register save, with a full switch only if the handler
blocks.  That removes two switches per soft interrupt, and bounds
the dispatch delay by how long interrupts stay disabled rather than
by how long a syscall runs.

The cost lands system-wide rather than on one subsystem:

- every `callout(9)`, which is every userland timer, sleep timeout
  and TCP timer;
- every USB transfer completion — `pusbhc` posts each one to a
  softint, so `ure(4)` networking and `umass` storage pay it;
- network input.  The inbound half of
  [the ping asymmetry](#netbsd-outbound-ping-sees-3x-the-latency-of-inbound-replies)
  runs on this path.

None of it is measured.  On the board, `vmstat -e | grep softint`
gives dispatches per level and `vmstat 1` the switch rate; two
switches per softint at the measured switch cost is the saving.  A
consumer that needs periodic timing, such as a tracker tick driven
from `callout(9)`, also wants the delay from the clock interrupt to
the callout, measured under load.

### Why the port does not have it

The fast path is opt-in: the generic one needs no machine-dependent
code, bring-up used it, and nothing has needed more.  Two properties
of the port make the fast path more than a copy of the reference
port's:

- **No software interrupt line.**  MIPS raises pending softints
  through CP0's software interrupt bits, which the hardware delivers
  once the interrupt mask allows them, so its dispatch needs no
  software check.  Penumbra has one interrupt-enable bit, so pending
  softints have to be checked wherever interrupts come back on: at
  the end of `trap()` when the interrupted context had them enabled,
  and in `splx(IPL_NONE)` and `spl0()`.  RISC-V dispatches this way
  (`cpu_dosoftints()`, `riscv/riscv/softint_machdep.c`) and is the
  closer template.
- **Binary IPL.**  Every level above `IPL_NONE` is `DI`, and the
  current level is read from `SR.I` rather than kept in a variable,
  because exception entry would desynchronize a variable from the
  hardware.  The dispatcher has to know which soft level is running,
  so that the next interrupt exit neither re-enters a softclock
  handler nor preempts it with a lower level.  That takes a
  variable, but only for the soft level: the dispatcher alone writes
  it and the hardware never changes it, so the desync cannot arise.

### What it takes

All machine-dependent, with no MI changes:

- `__HAVE_FAST_SOFTINTS` in `types.h`; per-CPU softint LWPs, a
  pending mask and the current soft level in `cpu_info`.
- `softint_init_md` and `softint_trigger`, a few lines each.
- A dispatch loop in C, shaped like RISC-V's `softint_deliver`.
- An assembly switch (MIPS's `softint_fast_dispatch` is about 110
  lines): save the pinned LWP's callee-saved registers into its PCB
  with the return address at a cleanup trampoline, move `SP` to the
  softint LWP's stack, set `curlwp` in both `cpu_info` and `R12`,
  call `softint_dispatch(pinned, s)`, and switch back.
- The trampoline: if a handler blocks, `cpu_switchto` later resumes
  the pinned LWP there, and it finishes the interrupt exit as if the
  dispatch had returned.
- A pending check in every `splx` that re-enables interrupts.

At run time that check is the whole cost; the real cost is risk.  The
dispatch sits on the trap-exit path, under the vector-page epilogue
rules, and mistakes there show up as hangs rather than panics.

## Build system: guard flashing against marginal timing

`make flash` packs and flashes whatever nextpnr produced, including a
build that missed its frequency constraint (`--timing-allow-fail`) or
one that passed with near-zero margin — and a pinned placement seed
goes stale the moment the netlist changes, so a routine RTL edit can
silently produce a 25.07-MHz-on-25 bitstream whose register-tier
flakiness then masquerades as logic bugs.  Add a guard: after PnR,
compare the achieved CPU-clock fmax against the constraint plus the
project's safety floor; refuse to pack/flash below it unless
explicitly overridden, and print the figure either way.  Pair it with
the working rule that any RTL change re-runs the seed sweep before
the pin is trusted.

## Board workflow: flashing, reset, and persistence annoyances

Four friction points in the daily flash-and-test loop, roughly in
order of pain:

- **Persistent default bitstream.**  Every power cycle currently
  loses the FPGA configuration and needs a fresh `make flash` (an
  SRAM load).  The ULX3S SPI configuration flash can hold a
  bitstream the ECP5 loads at power-on; teach the flow to program it
  (fujprog writes flash with the right flag) — likely as a separate
  `make flash-persistent` target so the fast SRAM load stays the
  development default.

- **SD card wedged by reset.**  A board reset or reflash that lands
  mid-SPI-transaction parks the SD card in a state the ROM's init
  cannot recover — only a power cycle clears it.  Debug the recovery
  path: the SD-SPI init sequence should be able to rescue a card
  abandoned mid-transaction (dummy clocks with CS deasserted, CMD0
  retry discipline, and if need be a longer pre-init clocking burst)
  instead of forcing the Vcc cycle.

- **Multiple bitstreams in flash, selectable at boot.**  The ECP5
  supports multiboot configurations in SPI flash (golden/update
  images with jump records selecting which loads).  Investigate what
  the open toolchain (ecppack/ecpmulti) supports and whether a
  button-selected boot image is achievable on the ULX3S — would let
  a known-good image and an experimental image coexist.

- **Reset-button press needed after flash.**  After an SRAM-load
  flash the design comes up needing a manual `btn[1]` press before
  it runs correctly.  Investigate why the post-configuration state
  differs from the post-reset state — likely something in the top's
  reset generation (PLL lock vs. release timing, or state the reset
  clears that configuration does not) — and make configuration
  come up clean.

## NetBSD: MD inline bswap existed but was never wired in — RESOLVED

`SHA256_Transform` (libc `sha2.c`, which NetBSD's libcrypto also links
as `libc-sha256`) called out-of-line `__bswap32` once per message word
— a call plus spill traffic where an inline shift/or expansion is
straight-line ALU; `htonl`/`ntohl` across the network stack (kernel
and userland) paid the same call.  Root cause was a wiring omission,
not a missing implementation: `penumbra/byte_swap.h` has had correct
`__BYTE_SWAP_U16/U32_VARIABLE` inlines since port bring-up, but
`penumbra/bswap.h` never included it, so `sys/bswap.h` fell back to
the libc functions.  Fixed by including it as
`<machine/byte_swap.h>`, `_LOCORE`-guarded — the i386/vax single-arch
convention, not riscv's `<riscv/...>` arch-name form: only kernel and
userland builds provide the arch-name include alias, while stand
(bootloader) builds alias `machine/` alone, so the arch-name form
breaks `stand/boot`.  `byte_swap.h` also switched
from `sys/types.h` to `sys/cdefs.h`+`sys/stdint.h` — the heavyweight
include recursed through `sys/endian.h`, whose inline codecs need
`bswap32` before `sys/bswap.h`'s declarations exist when the bswap
header is the entry point.  Probe-verified both include orders inline
to ~13 instructions with constants folding; remaining validation is a
world rebuild confirming `sha2.o` loses the `bl __bswap32`.

## NetBSD/OpenSSL: hash message ingestion byte-gathers even when aligned

Word loads must be 4-aligned (`VEC_ALIGN`), so the portable
byte-assembly in OpenSSL's `HOST_c2l` (MD5: 4×`ldb`+3×`shl`+3×`or` =
10 instructions per word, 160 per block ≈ 14% of the block cost) and
in libc sha2's `be32dec` path is semantically forced for arbitrary
`unsigned char *` input — the compiler cannot legally merge it.  A
source-level aligned fast path (word loads when `(p & 3) == 0`, which
is the overwhelmingly common case for hash input buffers) is the same
dispatch a hand-written implementation would use.  Decide per-library
whether the reachover/MD configuration offers a clean hook before
touching dist code.

## Compiler: Ch()-style bit-select not folded to the masked-merge form

In compiled `SHA256_Transform`, `Ch(e,f,g) = (e&f) ^ (~e&g)` emits the
naive `not`+2×`and`+`or` sequence (5 ops plus copies) while MD5's
equivalent `F(b,c,d)` correctly gets the masked-merge form
`d^(b&(c^d))` (3 ops plus one copy).  Both should canonicalize to the
xor form (InstCombine's bit-select fold); find where the SHA shape
escapes it — likely the `^` vs `|` variant or a disjoint-or rewrite
upstream of the fold.  Worth ~4 instructions × 64 rounds per block.

## ISA (data point): no rotate instruction costs SHA-256 ~30% of its ALU work

Every `ROTL(x,n)` lowers to the optimal-but-long `mov`+`shl`+`shr`+`or`
(4 ops; hand asm needs the same on a 2-address ISA).  SHA-256 executes
~480 rotates per 64-byte block (6 per round + 2 per schedule word) ≈
1900 instructions; MD5 64, SHA-1 ~160.  A Format-L `ROT Rd, #imm5`
through the existing barrel shifter would cut hash cores by roughly a
quarter to a third.  Recorded as a measurement, not a proposal — an ISA
change has its own bar (doc-first, conformance suite, all cores).

## Compiler: the pipeline prices MUL/DIV as if they cost one cycle — RESOLVED

`PenumbraTTIImpl::getArithmeticInstrCost` prices them at the tuned
core's scheduling-model latencies, and `isIntDivCheap()` weighs the
same figures (see
[the constant strength-reduction entry](#compiler-constant-multiply-divide-and-remainder-strength-reduction)).
The analysis below predates both.  Its closing claim that
`isIntDivCheap()` should stay true held only while every core's
multiply was as slow as its divide.

The divmul unit runs a fixed 32-iteration shift-add — no early-out, so
every `MUL`/`DIV` is a full multi-cycle stall regardless of operand
values.  Nothing in the compiler knows this.  `PenumbraTTIImpl` does not
override `getArithmeticInstrCost`, so `BasicTTIImplBase` reports the
same cost as an `ADD` for any legal operation, and the scheduling model
(`CompleteModel = 0`, no `WriteRes` entries) models the same.  Every
cost-driven decision — unrolling, LSR, SimplifyCFG speculation,
inlining, if-conversion — is therefore made believing a divmul is free.

Overriding `getArithmeticInstrCost` for s32 MUL/DIV/REM is the small,
broad fix.  Report the real cost for `TCK_Latency` and
`TCK_RecipThroughput` but leave `TCK_CodeSize` at 1: a `MUL` genuinely
*is* one instruction, and inflating its code-size cost would push the
unroller and inliner the wrong way on a core that already spends ~34%
of fractal-demo cycles stalled on instruction fetch.

`isIntDivCheap()` — the one hook that *is* set — remains correct, and
should not be flipped when the cost model lands.  Magic-multiply
division needs only the high half, so it is one `MULU_P`: the same
divmul latency as the `DIVU` it would replace, plus fixup shifts.
Modulo is worse, needing the divide plus a multiply-back and subtract.
The divide side is a genuine wash because our multiply is exactly as
slow as our divide; the asymmetry is entirely on the multiply side.

## Compiler: constant multiplies below the 2^n +/- 1 cases hit the divmul unit — RESOLVED

The post-legalizer `penumbra_mul_by_const` rule decomposes constant
multiplies against the tuned core's costs; see
[the constant strength-reduction entry](#compiler-constant-multiply-divide-and-remainder-strength-reduction).
The analysis below predates it.

Custom strength reduction in `PenumbraLegalizerInfo` covers `2^n` and
`2^n +/- 1` (so `x*3`, `x*5` become shift+add) and falls through to a
hardware `MUL` for everything else.  That cap is the right one for a
target whose multiply costs a few cycles — which is what the rule was
calibrated against, before the divmul unit existed — but here the
budget for replacing a constant multiply is the unit's full latency:
roughly 11 instructions even in a fetch-bound loop, more when fetch is
warm.  We spend at most two.

Missed cases are ordinary: `x*6`, `x*10`, `x*12`, `x*30`, `x*100` all
emit `MUL` where three to five shift/add/sub instructions suffice.
`x*320` is the one to note — a framebuffer row stride, so any
`fb[y*320+x]` that LSR does not convert into an induction variable
(`putpixel`, random-access blits) pays a full divmul stall per call.

A greedy shift/add decomposition over the constant (canonical signed
digit form handles the `-` cases like `30 = 32-2`) bounded at a few
instructions would cover essentially every small constant in real code.
Two traps for whoever picks this up: `TargetLowering::
decomposeMulByConstant` looks like the intended hook and is implemented
by RISC-V, X86, M68k, PowerPC, LoongArch, CSKY and Xtensa, but it is
called only from `DAGCombiner` and is dead under GlobalISel; and GISel's
own upstream coverage is just `mul_to_shl` (power-of-2), so there is no
rule to enable.  The work belongs in the existing custom MUL
legalization next to the `2^n +/- 1` cases, or in a post-legalizer
combine.

## Demos: mandelbrot's palette length forces a divide per cell

`pixel_color()` indexes `palette[iter % PALETTE_LEN]` with
`PALETTE_LEN` 30, so each cell pays two constant remainders
(foreground and background): two `DIVU` operations on penumbra1, and
on the default tuning a high-half multiply plus a shift/subtract
sequence each. Padding the table to 32 entries turns the index into
`and r1, 31`. Negligible in the terminal renderer — the escape loop
dwarfs it — but the cost scales with pixels rather than iterations, so
it grows in relative terms on a framebuffer, where shallow escape
counts are common.

## Hardware: audio device (`CLASS_AUDIO`)

The ULX3S has a 3.5 mm jack driven by a 4-bit resistor ladder per
channel, and nothing in the design uses it. Register interface and
driver model are specified in
[devices/audio.md](system/devices/audio.md); `CLASS_AUDIO` is registered
as class 7. This entry records the measurements behind the shape of that
spec and the decisions that still need hardware to settle.

### Why the device has no addressable sample memory

The bus expresses exactly one master. Every control signal in
[bus-protocol.md](hardware/bus-protocol.md) is marked master-to-slave and
there is no request/grant pair, so a device cannot fetch its own sample
data; anything it plays, the CPU must push. That leaves two shapes: a
CPU-filled BRAM the channels address (Paula/GUS), or per-channel FIFOs.

Sample-header survey of 50 four-channel modules across ten composers
(4-Mat, Captain, Dizzy, Heatbeat, Jester, Jogeir Liljedahl, Lizardking,
Mantronix, Moby, Romeo Knight), total sample bytes per module:

```
median=90K  mean=113K  max=382K
  fits in  32K: 14/50 (28%)     Jester/elysium        102K
  fits in  64K: 19/50 (38%)     4-Mat/crocketts theme  88K
  fits in 128K: 34/50 (68%)     Heatbeat/bicykl0pedia 382K
  fits in 384K: 50/50 (100%)
```

Addressable sample memory is therefore not worth its area: 64 KB (32
EBR) plays 38% of real modules, and 128 KB reaches only 68% while taking
EBR occupancy to ~95%. Per-channel FIFOs play all of them, at ~16 EBR
and roughly 1% of the CPU — four channels at the maximum Amiga rate
(period 108, ~33 kHz) is 132 KB/s, pushed as words holding four 8-bit
samples each.

The cost is that FIFO contents belong to one source stream at one rate,
so a change of source material needs `FLUSH` rather than a queued
handoff. A change of *pitch* needs nothing: stream position is the
integral of `INC` over elapsed time, so a queued channel and an
unqueued one behave identically under vibrato or portamento.

### The DAC ladder compensates for the FPGA pin's own resistance

Ladder values from the board schematic (R14-R17 per channel): 130 Ω
(MSB), 270 Ω, 549 Ω, 1.1 kΩ. The same sheet states the pin's own
output resistance as 10 Ω and derives the board's 75 Ω figure from it.
That 10 Ω is part of the design: including it the legs are
140/280/559/1110 against an ideal binary 140/280/560/1120, for a
worst-case DNL of ±0.013 LSB. The odd-looking 549 and 1.1 k exist so
the sum lands on the binary sequence; computing weights from the bare
resistor values instead yields +0.34 LSB and a ladder that only looks
mismatched.

The compensation is exact at one pin resistance — worst-case DNL runs
+0.336 at 0 Ω, ±0.013 at 10 Ω, −0.332 at 20 Ω — and the ECP5's drive
impedance moves with process, voltage and temperature. Worse, it
differs between sourcing and sinking, which makes each leg's weight
depend on that leg's own state. No resistor value trims that out.

Noise shaping does not fix ladder nonlinearity: the static error acts
on the code stream unshaped, intermodulating out-of-band shaped noise
back into the audio band. Quantization itself is a non-issue —
second-order shaping at OSR 512 puts it ~148 dB down.

### Simulated: leg choice matters more than code width

`make audio-sweep` drives the modulator against a model of the ladder
with the pin's source and sink resistance swept. The code stream does
not depend on what the ladder does with it, so one capture feeds every
parameter point. Stimulus is a two-tone pair into 10 kΩ; the
difference product exists only through nonlinearity.

Two results survived contact with hardware: a one-bit code is exactly
invariant under any asymmetry (two levels define a straight line), and
which legs a code drives matters more than how many, because the pin
resistance perturbs each leg in proportion to its conductance — ±2 Ω
is 1.4% of the 140 Ω leg and 0.18% of the 1110 Ω one.

The magnitudes did not: the model was 12 dB optimistic on the four-bit
case, and predicted the wrong winner. It has no term for switching
current, which hardware showed dominates both the distortion and the
noise. Treat it as a ranking tool, not a predictor.

### Shortening the modulator loop to close 60 MHz

The loop was two serial 20-bit adds plus two saturations and the
quantizer: 18.95 ns against a 16.67 ns period, 52.8 MHz achieved. Four
changes took it to 12.88 ns and 77.6 MHz, none of them altering the
transfer function or moving the measured figures by 0.4 dB:

- the DAC reconstruction is a sixteen-entry table rather than a shift
  and a subtract, the code being at most four bits;
- the quantizer's offset lives in v2's reset value, since a constant
  added to both sides of an integrator's recurrence cancels;
- the input scale is registered, being feedforward rather than part of
  the loop;
- v2 subtracts the feedback from its own registered state in parallel
  with v1's sum, rather than behind it.

Latency is what made three of those free: at these rates a sample can
be delayed by a hundred clocks and remain invisible, so anything not
inside the feedback path can be registered at will. Only the loop
itself — v1 and v2 through the quantizer and back — has to stay
combinational, and that is what the four changes shortened.

This matters because the sample rate comes off the 60 MHz USB clock
precisely so it does not follow a retuned system clock.

### Ladder pins are registered at the pads, and it changes nothing

The four legs must switch together: skew between them would present
the ladder a code that was never commanded, and that glitch energy is
code-correlated, which a measurement cannot tell from nonlinearity.
Registering at the pads packs the registers into the IO cells and took
the worst output path from 6.37 ns over eight hops to 2.5 ns over two.

It moves no measured figure — the four-bit configuration reads −40.6 dB
third-order either way, to the decimal place, with the same noise
floor. Skew is therefore not a contributor, which is the third
mechanism excluded by measurement rather than argument. The registers
stay for the output timing they give, not as a fix.

### Measured on hardware: two bits on 549/1.1k

Seven configurations recorded through the jack into a line input,
two-tone stimulus (9000 + 10125 Hz), 25 s captures at 0.084 Hz
resolution, sorted by the figure that decided it. "Noise" is absolute
in-band; "matched SNR" is what a listener hears once the
configurations are level-matched.

| config | peak leg mA | 3rd-order | noise | matched SNR |
|--------|-------------|-----------|-------|-------------|
| 2b {549,1.1k}     |  5.9 | −87.4 | −128.8 | **108.8** |
| 1b {1.1k}         |  3.0 | −83.4 | −134.5 | 105.2 |
| 2b {270,549}      | 11.8 | −89.1 | −123.3 | 103.3 |
| 3b {270,549,1.1k} | 11.8 | −53.7 | −123.1 | 103.1 |
| 1b {130}          | 23.6 | −79.4 | −120.8 | 100.8 |
| 4b {all}          | 23.6 | −40.6 | −120.6 | 100.6 |
| 3b {130,270,549}  | 23.6 | −50.4 | −119.8 |  99.8 |

Distortion is measured on the **third-order** pair (2f1−f2, 2f2−f1).
Ladder mismatch is odd-order; the recording chain's own distortion is
even-order and lands on f2−f1 and f1+f2, so the converter can be
measured through a chain far dirtier than itself. Reading the
difference tone as the converter's condemns every configuration alike
at about −45 dB. The one-bit rows are the floor — their static
transfer function cannot bend — and both two-bit configurations
measure below them.

**Two bits is the ceiling.** The third bit costs 35 dB, a cliff rather
than a gradient, and far more than any weight-ratio error produces.
Two candidate mechanisms are excluded by measurement: the same
configuration reads identically at 30 MHz and 60 MHz, which rules out
settling and edge asymmetry; and a two-bit converter on the 270 Ω leg
is among the cleanest, which rules out one bad leg.

**Noise decides it, not distortion.** Absolute in-band noise rises
about 5.7 dB per doubling of the largest driven leg's current,
monotonically across all seven. It is not quantization noise — three
configurations share a 0.22 V step and span 14 dB of noise, and at
this oversampling even one bit has ~135 dB of in-band SQNR. Through
amplified speakers the two configurations that avoid the 130 Ω and
270 Ω legs are audibly clean and the rest hiss; no configuration
sounds *bad*, so the choice is between margins.

Two measurement notes that inverted conclusions. Noise relative to the
tones ranks the configurations backwards, because each was captured at
its own span; only absolute figures are monotonic. And level-matching
is what exposes the noise at all, since digital attenuation scales the
signal and not the analog floor — so the probe scales the sample clip
per configuration and deliberately leaves the measurement tones alone.

Design consequence: **two bits on 549/1.1k**, driving neither
high-current leg. 0.66 V p-p (234 mV RMS) is line level unaided. Two
bits on 270/549 measures 1.7 dB better on distortion and is the wrong
choice: its 270 Ω leg costs 5.5 dB of noise.

### The jack is DC coupled, and the intended load is line level

There is no series capacitor between ladder and jack — the schematic
shows only R14-R17 in the path, and the board manual describes the
output as "3-channel 4-bit DAC with 75 ohm output impedance and 0-3.3V
DC swing".

The intended load is not headphones. Exhibit use drives amplified
speakers or the line input of a capture card, both high-impedance and
AC-coupled at the receiver. Three consequences:

- **DC is a non-issue.** Into 10 kΩ the midscale rest level draws
  0.16 mA and the receiver blocks it. Into 16 Ω earphones the same
  rest level would put ~18 mA and ~5 mW of DC through the transducer,
  which is why `CTRL.ENABLE = 0` driving the pins low rather than
  parking at midscale is worth keeping as an option.
- **Full scale is hot.** 3.3 V p-p is 1.17 V RMS, about 13 dB above
  the −10 dBV consumer nominal, so a sensible operating level sits
  well below digital full scale. The headroom is free: the modulator
  measures 96 dB SNR at four bits.
- **Out-of-band energy matters more**, not less. A transducer filters
  it; a line input does not, and the board has no reconstruction
  filter. This is the second argument against narrow codes.

### Clock source must not follow CPU_HZ

Sample rate cannot derive from the CPU/system clock, which is retuned
per timing closure. The USB tier already solves this shape —
`ecp5_pll_aux_div` returns 0 unless the VCO is an exact multiple, so an
inexact retarget fails elaboration instead of detuning the peripheral.

Both instantiated PLLs are constrained, and their VCO factorisations
decide what is reachable:

| clock             | factorisation  | exact 48 kHz? |
|-------------------|----------------|---------------|
| system VCO / USB 60 MHz | 2⁸ × 3 × 5⁷ | yes (÷1250) |
| video VCO 625 MHz | 2⁶ × 5¹⁰       | no            |
| *48000*           | *2⁷ × 3 × 5³*  | —             |

The video PLL has two free outputs (`ENCLKOS2`/`ENCLKOS3` tied off) and
is crystal-fed, so it looks like the obvious home — but 625 MHz has no
factor of 3 and only 2⁶, so it cannot produce 16/32/44.1/48 kHz by any
downstream divider. It reaches 8 kHz exactly, and otherwise only
oddities like 25/40/50 kHz.

Preferred: share the existing 60 MHz USB clock, which adds no new VCO
constraint because USB already imposes it, and divides exactly across
the whole 48 kHz family (8/16/24/32/48/96 kHz). Portable fallback for a
top without a USB tier: a third EHXPLLL (2 of 4 used), crystal-fed at a
600 MHz VCO — 2⁹ × 3 × 5⁸, reachable from 25 MHz as CLKI 1 / CLKFB 24,
and exact at ÷12500. 44.1 kHz factors as 2² × 3² × 5² × 7² and is
unreachable from any of these; design around the 48 kHz family.

### Sizing, and what needs re-measuring

Last full synthesis showed `DP16KD 134/208` — 74 EBR (~148 KB) free,
`MULT18X18D 0/156`, `EHXPLLL 2/4`. Eight channels of 4 KB is 16 EBR,
taking occupancy to ~72%. Audio RAM is read at the output rate, touches
no critical path, and can place loosely, unlike the floorplanned SDRAM
and D-side cones.

- The EBR figure predates in-flight L2 geometry changes and should be
  re-measured before it is planned against.
- A 4 KB FIFO holds 124 ms for a channel draining at 33 KB/s but only
  21 ms for a 48 kHz stereo stream at 192 KB/s. Either give channel 0 a
  deeper FIFO — `DEPTH` is per channel precisely so this is legal and
  discoverable — or set `__AUDIO_BLK_MS` low enough that two blocks fit.
- The mixer's multiply-accumulate can be time-multiplexed across all
  channels: 1250 clocks per output frame at 60 MHz against roughly four
  clocks per channel. Channel count is bounded by register-file size,
  not by mixing throughput, so `INTERP` (one extra MAC per channel) is
  affordable rather than a luxury.

### Driver-side notes

A tracker refills reactively — `used = DEPTH - LEVEL`, top up to a
target — which needs no rate arithmetic and self-corrects against tick
jitter. Ordering inside the tick handler is load-bearing: apply effects
to `INC` *first*, then read `LEVEL`, so the top-up already reflects the
new drain rate. Arpeggio can raise pitch 15 semitones (2.38×) on the
next tick, so a target of one tick's data underflows; ~1500 frames is
over two ticks even at the maximum rate and needs no per-channel
computation.

Multi-channel control has no representation in `audio(4)` and reaches
userland through the framework's `dev_ioctl` hook. `midisyn` is not an
alternative: it speaks MIDI note numbers and centibel attenuations, has
no path for loading arbitrary samples, and its `repitchv`/`relevelv`
methods — continuous pitch and level change, i.e. most of a tracker's
effect vocabulary — are unimplemented.

### Bring-up order

The two halves are independently testable, so neither blocks the other
for long:

1. Output stage alone — a synthesized signal into the modulator and
   ladder, `CTRL.ENABLE` only, needing no FIFO, no channels and no
   driver. What it confirms is the [asymmetry
   tolerance](#the-dac-ladder-compensates-for-the-fpga-pins-own-resistance):
   a two-tone stimulus, because a difference tone appears only through
   nonlinearity and is unmistakable where harmonic distortion on a
   single tone is not, and silence, for the noise floor.
2. One channel with a FIFO, plus the `audio(4)` binding. That is the
   whole `CLASS_AUDIO` minimum protocol and gives a working
   `/dev/audio`.
3. Remaining channels, `INC`, `VOL` and the mixer — the point at which
   `CAP.CHANNELS` becomes greater than one and the
   [ioctl interface](system/netbsd/design-notes.md#audio-driver-multi-channel-ioctl-interface)
   becomes reachable.
4. A tracker replayer in userland, with its mixer removed and channel
   registers driven in its place.

Step 1 is worth doing before any of the register interface exists, but
most of it is a simulation rather than a listening test: the modulator's
code stream does not depend on the ladder, so one captured stream feeds
a swept model of the analog mapping. Hardware is needed only for what
the model cannot supply — the ECP5's actual source/sink impedance.

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
